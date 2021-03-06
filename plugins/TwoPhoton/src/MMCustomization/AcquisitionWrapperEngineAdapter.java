/*
 * To change this template, choose Tools | Templates
 * and open the template in the editor.
 */
package MMCustomization;

import com.imaging100x.twophoton.SettingsDialog;
import java.io.File;
import java.io.IOException;
import java.util.concurrent.BlockingQueue;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.prefs.Preferences;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import javax.swing.JOptionPane;
import mmcorej.TaggedImage;
import org.json.JSONException;
import org.json.JSONObject;
import org.micromanager.AcqControlDlg;
import org.micromanager.AcquisitionEngine2010;
import org.micromanager.acquisition.AcquisitionWrapperEngine;
import org.micromanager.utils.JavaUtils;
import org.micromanager.MMStudioMainFrame;
import org.micromanager.acquisition.AcquisitionManager;
import org.micromanager.acquisition.DefaultTaggedImageSink;
import org.micromanager.acquisition.MMImageCache;
import org.micromanager.api.IAcquisitionEngine2010;
import org.micromanager.api.SequenceSettings;
import org.micromanager.api.TaggedImageStorage;
import org.micromanager.utils.MDUtils;
import org.micromanager.utils.ReportingUtils;


/**
 * Adapter class that sends commands in MM from acqWrapperEngine interface to the real acquisition engine
 * created within the plugin
 * @author Henry
 */
public class AcquisitionWrapperEngineAdapter extends AcquisitionWrapperEngine {

   private IAcquisitionEngine2010 realAcqEng_;
      private Preferences prefs_;

   
   public AcquisitionWrapperEngineAdapter(Runnable depthListRunnable, Preferences prefs) throws NoSuchFieldException {
      super((AcquisitionManager) JavaUtils.getRestrictedFieldValue(
              MMStudioMainFrame.getInstance(), MMStudioMainFrame.class, "acqMgr_"));
      MMStudioMainFrame gui = MMStudioMainFrame.getInstance();
      super.setCore(MMStudioMainFrame.getInstance().getCore(), null);


      prefs_ = prefs;
      realAcqEng_ = new AcquisitionEngine2010(MMStudioMainFrame.getInstance().getCore());
      realAcqEng_.attachRunnable(-1, -1, -1, -1, depthListRunnable);
      try {
         gui.setAcquisitionEngine(this);
         //assorted things copied from MMStudioMainFrame to ensure compatibility
         this.setParentGUI(gui);
         this.setZStageDevice(gui.getCore().getFocusDevice());
         this.setPositionList(gui.getPositionList());
         JavaUtils.setRestrictedFieldValue(gui.getAcqDlg(), AcqControlDlg.class, "acqEng_", this);
      } catch (Exception ex) {
         ReportingUtils.showError("Could't override acquisition engine");
      }
   }

   private void runAcquisition(SequenceSettings settings) throws Exception {
      // Start up the acquisition engine
      BlockingQueue<TaggedImage> engineOutputQueue = realAcqEng_.run(settings, true, gui_.getPositionList(), null);
      JSONObject summaryMetadata = realAcqEng_.getSummaryMetadata();

      // Set up the DataProcessor<TaggedImage> sequence--no data processors for now
      // BlockingQueue<TaggedImage> procStackOutputQueue = ProcessorStack.run(engineOutputQueue, imageProcessors);
      
      // create storage
      TaggedImageStorage storage;
      try {
         if (settings.save) {
            //MPTiff storage
            String acqDirectory = createAcqDirectory(summaryMetadata.getString("Directory"), summaryMetadata.getString("Prefix"));
            summaryMetadata.put("Prefix", acqDirectory);
            String acqPath = summaryMetadata.getString("Directory") + File.separator + acqDirectory;
            storage = new DoubleTaggedImageStorage(summaryMetadata, prefs_.getBoolean(SettingsDialog.INVERT_X, false), prefs_.getBoolean(SettingsDialog.INVERT_Y, false),
                    prefs_.getBoolean(SettingsDialog.SWAP_X_AND_Y, false), acqPath, prefs_.get(SettingsDialog.STITCHED_DATA_DIRECTORY, ""));
         } else {
            //RAM storage
            storage = new DoubleTaggedImageStorage(summaryMetadata, prefs_.getBoolean(SettingsDialog.INVERT_X, false), prefs_.getBoolean(SettingsDialog.INVERT_Y, false),
                    prefs_.getBoolean(SettingsDialog.SWAP_X_AND_Y, false), null, prefs_.get(SettingsDialog.STITCHED_DATA_DIRECTORY, ""));
         }

         MMImageCache imageCache = new MMImageCache(storage) {
            @Override
            public JSONObject getLastImageTags() {
               //So that display doesnt show a position scrollbar when imaging finished
               JSONObject newTags = null;
               try {
                  newTags = new JSONObject(super.getLastImageTags().toString());
                  MDUtils.setPositionIndex(newTags, 0);
               } catch (JSONException ex) {
                  ReportingUtils.showError("Unexpected JSON Error");
               }
               return newTags;
            }
         };
         imageCache.setSummaryMetadata(summaryMetadata);
         
                    
         DisplayPlus stitchedDisplay = new DisplayPlus(imageCache, this, summaryMetadata, 
                 prefs_.getBoolean(SettingsDialog.INVERT_X, false), prefs_.getBoolean(SettingsDialog.INVERT_Y, false),
                  prefs_.getBoolean(SettingsDialog.SWAP_X_AND_Y, false));

         DefaultTaggedImageSink sink = new DefaultTaggedImageSink(engineOutputQueue, imageCache);
         sink.start();

                       
      } catch (IOException ex) {
         ReportingUtils.showError("Couldn't create image storage or start acquisition");
      }
   }
   
    //Copied from MMAcquisition
   private String createAcqDirectory(String root, String prefix) throws Exception {
      File rootDir = JavaUtils.createDirectory(root);
      int curIndex = getCurrentMaxDirIndex(rootDir, prefix + "_");
      return prefix + "_" + (1 + curIndex);
   }

   private int getCurrentMaxDirIndex(File rootDir, String prefix) throws NumberFormatException {
      int maxNumber = 0;
      int number;
      String theName;
      for (File acqDir : rootDir.listFiles()) {
         theName = acqDir.getName();
         if (theName.startsWith(prefix)) {
            try {
               //e.g.: "blah_32.ome.tiff"
               Pattern p = Pattern.compile("\\Q" + prefix + "\\E" + "(\\d+).*+");
               Matcher m = p.matcher(theName);
               if (m.matches()) {
                  number = Integer.parseInt(m.group(1));
                  if (number >= maxNumber) {
                     maxNumber = number;
                  }
               }
            } catch (NumberFormatException e) {
            } // Do nothing.
         }
      }
      return maxNumber;
   }
   
   
   @Override
   protected String runAcquisition(SequenceSettings acquisitionSettings, AcquisitionManager acqManager) {     
      if (acquisitionSettings.save) {
         File root = new File(acquisitionSettings.root);
         if (!root.canWrite()) {
            int result = JOptionPane.showConfirmDialog(null, "The specified root directory\n" + root.getAbsolutePath() +"\ndoes not exist. Create it?", "Directory not found.", JOptionPane.YES_NO_OPTION);
            if (result == JOptionPane.YES_OPTION) {
               root.mkdirs();
               if (!root.canWrite()) {
                  ReportingUtils.showError("Unable to save data to selected location: check that location exists.\nAcquisition canceled.");
                  return null;
               }
            } else {
               ReportingUtils.showMessage("Acquisition canceled.");
               return null;
            }
         } else if (!this.enoughDiskSpace()) {
            ReportingUtils.showError("Not enough space on disk to save the requested image set; acquisition canceled.");
            return null;
         }
      }
      try {
         //run acquisition with our acquisition engine
         runAcquisition(acquisitionSettings);
      } catch (Exception ex) {
         ReportingUtils.showError("Probelem running acquisiton: " + ex.getMessage());
      }
      
      return "";
   } 
   
   @Override
   public void setPause(boolean pause) {
      if (pause) {         
         realAcqEng_.pause();
      } else {
         realAcqEng_.resume();
      }
   }
   
   @Override
   public boolean isPaused() {
      return realAcqEng_.isPaused();
   }
   
   @Override
   public boolean abortRequest() {
      if (isAcquisitionRunning()) {
         int result = JOptionPane.showConfirmDialog(null,
                 "Abort current acquisition task?",
                 "Micro-Manager", JOptionPane.YES_NO_OPTION,
                 JOptionPane.INFORMATION_MESSAGE);

         if (result == JOptionPane.YES_OPTION) {
            realAcqEng_.stop();
            return true;
         }
      }
      return false;
   }

   @Override
   public boolean isAcquisitionRunning() {
      return realAcqEng_.isRunning();
   }

  @Override
  public long getNextWakeTime() {
     return realAcqEng_.nextWakeTime();
  }
  
  @Override
  public boolean abortRequested() {
     return realAcqEng_.stopHasBeenRequested();
  }

}
