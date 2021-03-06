///////////////////////////////////////////////////////////////////////////////
// FILE:          FastLogger.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     MMCore
//-----------------------------------------------------------------------------
// DESCRIPTION:   Implementation of the IMMLogger interface
// COPYRIGHT:     University of California, San Francisco, 2009
// LICENSE:       This file is distributed under the "Lesser GPL" (LGPL) license.
//                License text is included with the source distribution.
//
//                This file is distributed in the hope that it will be useful,
//                but WITHOUT ANY WARRANTY; without even the implied warranty
//                of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
//                IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//                CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
//                INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.
//
// AUTHOR:        Karl Hoover, karl.hoover@ucsf.edu, 2009 11 11
// 
// CVS:           $Id$




#include <stdarg.h>
#include <string>
#include <iostream>
#include <fstream>

#include <limits.h>

#include "FastLogger.h"
#include "CoreUtils.h"
#include "../MMDevice/DeviceUtils.h"
#include "boost/thread/thread.hpp"
#include "boost/interprocess/detail/os_thread_functions.hpp" 
#include "boost/version.hpp"
#if BOOST_VERSION >= 104800
#  define BOOST_IPC_DETAIL boost::interprocess::ipcdetail
#else
#  define BOOST_IPC_DETAIL boost::interprocess::detail
#endif

using namespace std;
const char* g_textLogIniFiled = "Logging initialization failed\n";

class LoggerThread : public MMDeviceThreadBase
{
   public:
      LoggerThread(FastLogger* l) : log_(l), busy_(false), stop_(false) {}
      ~LoggerThread() {}
      int svc (void);

      void Stop() {stop_ = true;}
      void Start() {stop_ = false; activate();}

   private:
      FastLogger* log_;
      bool busy_;
      bool stop_;
};


int LoggerThread::svc(void)
{
   do
   {
		std::string stmp;
		{
			MMThreadGuard stringGuard(log_->logStringLock_g);
			stmp = log_->stringToWrite_g;
			log_->stringToWrite_g.clear();
		}

		if (0 < stmp.length())
		{
			if( 0 != (log_->flags() & STDERR))
				std::cerr << stmp << '\n' << flush;
                                
			MMThreadGuard fileGuard(log_->logFileLock_g);
			if( NULL != log_->plogFile_g)
				*log_->plogFile_g << stmp << '\n' << flush;
		}
		CDeviceUtils::SleepMs(30);
   } while (!stop_ );

   return 0;
}


LoggerThread* pLogThread_g = NULL;


FastLogger::FastLogger()
:level_(any)
,fast_log_flags_(any)
,plogFile_(NULL)
,failureReported(false),
plogFile_g(0)
{
}

FastLogger::~FastLogger()
{
   if( NULL != pLogThread_g)
   {
      pLogThread_g->Stop();
      pLogThread_g->wait();
      delete pLogThread_g;
   }
   Shutdown();
}

/**
* methods declared in IMMLogger as pure virtual
* Refere to IMMLogger declaration
*/


bool FastLogger::Initialize(std::string logFileName, std::string logInstanceName)throw(IMMLogger::runtime_exception)
{
   bool bRet =false;
   try
   {
      failureReported=false;
      logInstanceName_=logInstanceName;
		{
			MMThreadGuard guard(logFileLock_g);
         bRet = Open(logFileName);
			if(bRet)
			{
				fast_log_flags_ |= OSTREAM;
			}
			fast_log_flags_ |= STDERR;
			set_flags (fast_log_flags_);
		}

      if( NULL == pLogThread_g)
      {
		   pLogThread_g = new LoggerThread(this);
		   pLogThread_g->Start();
      }

   }
   catch(...)
   {
      ReportLogFailure();
      throw(IMMLogger::runtime_exception(g_textLogIniFiled));
   }
   return bRet;
};


void FastLogger::Shutdown()throw(IMMLogger::runtime_exception)
{
   try
   {
      MMThreadGuard guard(logFileLock_g);
      failureReported=false;

      if(NULL != plogFile_g)
      {
         plogFile_g->close();
         delete plogFile_g;
         plogFile_g = NULL;
      }
      clr_flags (OSTREAM);
      //msg_ostream (0, 0);
   }
   catch(...)
   {
      plogFile_g = NULL;
      ReportLogFailure();
      throw(IMMLogger::runtime_exception(g_textLogIniFiled));
   }
}

bool FastLogger::Reset()throw(IMMLogger::runtime_exception)
{
   bool bRet =false;
   try{
		MMThreadGuard guard(logFileLock_g);
      failureReported=false;
      if(NULL != plogFile_g)
      {
         if (plogFile_g->is_open())
         {
            plogFile_g->close();
         }
         //re-open same file but truncate old log content
         plogFile_g->open(logFileName_.c_str(), ios_base::trunc);
         bRet = true;
      }
   }
   catch(...)
   {
      ReportLogFailure();
      throw(IMMLogger::runtime_exception(g_textLogIniFiled));
   }

   return bRet;
};

void FastLogger::SetPriorityLevel(IMMLogger::priority level) throw()
{
   level_ = level;
}

bool FastLogger::EnableLogToStderr(bool enable)throw()
{
   bool bRet = ( 0 != (fast_log_flags_ | STDERR));

   fast_log_flags_ |= OSTREAM;
   if (enable)
   {
      fast_log_flags_ |= STDERR;
   }
   else
   {
      fast_log_flags_ &= ~STDERR;
   }
   pLogThread_g->Stop();
   pLogThread_g->wait();
   set_flags (fast_log_flags_);
	pLogThread_g->Start();

   return bRet;
};


const int MaxBuf = 32767;

typedef struct 
	{
		char buffer[MaxBuf];
} BigBuffer;


void FastLogger::Log(IMMLogger::priority p, const char* format, ...) throw()
{

#ifdef _WINDOWS
	// let snprintf be OK
#pragma warning (disable:4996)  
#endif

	{
		MMThreadGuard guard(logFileLock_g);
		if( NULL == plogFile_g) 
		{
			cerr<< " log file is NULL!" << endl;
			return;
		}
	}

	try
	{
		// filter by current priority
      if (!(p & level_))
			return;

		std::string workingString;
		std::string formatPrefix = GetFormatPrefix(p);
		//InitializeInCurrentThread();
		// Start of variable args section
		va_list argp;

		va_start (argp, format);
		// build the string specfied by the variable argument list


		std::auto_ptr<BigBuffer> pB (new BigBuffer());
		//char buffer[MaxBuf];
		unsigned int flen = (unsigned int)strlen(format);

		// N.B. this loop increments the string iterator in two diffent conditions!
		for (unsigned int i = 0; format[i] != 0; ++i)
		{
			pB->buffer[0] = 0;
			
			if ('%' == format[i] ) // we found a printf format element
			{
				if( 0 == argp)
					break;
				// iterate over the format specifier
				char curc = format[++i];
				switch(curc) 
				{
					int n;
					float f;
					char c;
					char* s;

					case 'd':
						n = va_arg(argp, int);
						snprintf(pB->buffer, MaxBuf, "%d", n);
						break;

					case 'f':
						f = static_cast<float>(va_arg(argp, double));
						snprintf(pB->buffer, MaxBuf,"%f", f);
						break;

               case 'e':
						f = static_cast<float>(va_arg(argp, double));
						snprintf(pB->buffer, MaxBuf,"%e", f);
						break;

               case 'g':
						f = static_cast<float>(va_arg(argp, double));
						snprintf(pB->buffer, MaxBuf,"%g", f);
						break;

					case 'c':
#ifdef _WINDOWS
						c = va_arg(argp, char);
#else
						c = va_arg(argp, int);
#endif
						snprintf(pB->buffer, MaxBuf, "%c", c);
						break;

					case 's':
						s = va_arg(argp, char *);
						snprintf(pB->buffer, MaxBuf, "%s", s );
						break;

					case '.':   // width  (assume it's a floating point) TODO - other uses of width field
						f = static_cast<float>(va_arg(argp, double));
						{
							std::string newformat = "%";
							if( i < flen-1)
							{
								//char specwidth = '0';
								newformat += '.';
								newformat += format[i+1];
								newformat += 'f';
								i += 2;
							}
							snprintf( pB->buffer, MaxBuf, newformat.c_str(), f);
						}
						break;

					// process ID descriptor info ala ACE
					case 't':
					case 'D':
					case 'P':
						snprintf(pB->buffer, 2, "%s", "%");
						workingString += std::string(pB->buffer);
						snprintf(pB->buffer, 2, "%c", curc);
						break;

					default:
					break;
				}
				workingString += std::string(pB->buffer);
			}
			else
			{
				workingString+=format[i];

			}
			if( 0 == format[i]) // the format specifier was at the end of the format string
				break;
		}



		va_end(argp);

		workingString = formatPrefix + workingString;
		// the string specified on the argument list can also have the ACE %D tokens inside it
		//workingString += std::string(pB->buffer);
		
		///put the three replacements into a loop!
		// replace all %D with Date/Time
		// repleace all %t thread id
		// replacee all %P process id

		boost::posix_time::ptime bt = boost::posix_time::microsec_clock::local_time();

		std::string todaysDate = boost::posix_time::to_iso_extended_string(bt);


		size_t ifind = workingString.find("%D");
		while(std::string::npos != ifind)
		{
			workingString.replace(ifind,2,todaysDate);
			ifind =  workingString.find("%D");
		}

		std::ostringstream percenttReplacement;
      // Use the platform thread id where available, so that it can be compared
      // with debugger, etc.
#ifdef _WINDOWS_
      percenttReplacement << GetCurrentThreadId();
#else
		pthread_t pthreadInfo;
		pthreadInfo = pthread_self();
		percenttReplacement << pthreadInfo;
#endif

		ifind = workingString.find("%t");
		while(std::string::npos != ifind)
		{
			workingString.replace(ifind,2,percenttReplacement.str());
			ifind =  workingString.find("%t");
		}

		// display the process id
		BOOST_IPC_DETAIL::OS_process_id_t pidd = BOOST_IPC_DETAIL::get_current_process_id();

		std::ostringstream percentPReplacement;
		percentPReplacement << pidd;
		ifind = workingString.find("%P");
		while(std::string::npos != ifind)
		{
			workingString.replace(ifind,2,percentPReplacement.str());
			ifind =  workingString.find("%P");
		}

		if( '\n' == workingString.at(workingString.length()-1))
			workingString.replace(workingString.length()-1,1,"");



		{
			MMThreadGuard stringGuard(logStringLock_g);
			if ( 0 <stringToWrite_g.size())
				stringToWrite_g += '\n';
			stringToWrite_g += workingString;
		}



   }
   catch(...)
   {
      ReportLogFailure();
   }
#ifdef _WINDOWS
	// default for the warnings...
#pragma warning (default:4996  )
#endif

};

void FastLogger::ReportLogFailure()throw()
{
   if(!failureReported)
   {
      failureReported=true;

      MMThreadGuard guard(logFileLock_g);
      try {
         std::cerr << g_textLogIniFiled;
      }
      catch (...) {
      }
   }
};

#define CORE_DEBUG_PREFIX "%D p:%P t:%t [dbg] "
#define CORE_LOG_PREFIX "%D p:%P t:%t [LOG] "

const char * FastLogger::GetFormatPrefix(IMMLogger::priority p)
{
   if (p == debug)
      return CORE_DEBUG_PREFIX;
   return CORE_LOG_PREFIX;
}


bool FastLogger::Open(const std::string specifiedFile)
{

   bool bRet = false;
   try
   {
		if(NULL == plogFile_g)
		{
			plogFile_g = new std::ofstream();
		}
		if (!plogFile_g->is_open())
		{
         // N.B. we do NOT handle re-opening of the log file on a different path!!
         
         if(logFileName_.length() < 1) // if log file path has not yet been specified:
         {
            logFileName_ = specifiedFile;
         }

         // first try to open the specified file without any assumption about the path
	      plogFile_g->open(logFileName_.c_str(), ios_base::app);
         //std::cout << "first attempt to open  " << logFileName_.c_str() << (plogFile_g->is_open()?" OK":" FAILED")  << std::endl;

         // if the open failed, assume that this is because the ordinary user does not have write access to the application / program directory
	 if (!plogFile_g->is_open())
         {
            std::string homePath;
#ifdef _WINDOWS
            homePath = std::string(getenv("HOMEDRIVE")) + std::string(getenv("HOMEPATH")) + "\\";
#else
            homePath = std::string(getenv("HOME")) + "/";
#endif
            logFileName_ = homePath + specifiedFile;
				plogFile_g->open(logFileName_.c_str(), ios_base::app);
         }

		}
      else
      {
         ;//std::cout << "log file " << logFileName_.c_str() << " was open already" << std::endl;
      }

      bRet = plogFile_g->is_open();
   }
   catch(...){}
   return bRet;

}

void FastLogger::LogContents(char** ppContents, unsigned long& len)
{
   *ppContents = 0;
   len = 0;

   MMThreadGuard guard(logFileLock_g);

   if (plogFile_g->is_open())
   {
      plogFile_g->close();
   }

   // open to read, and position at the end of the file
   // XXX We simply return NULL if cannot open file or size is too large!
   std::ifstream ifile(logFileName_.c_str(), ios::in | ios::binary | ios::ate);
   if (ifile.is_open())
   {
      std::ifstream::pos_type pos = ifile.tellg();

      // XXX This is broken (sort of): on 64-bit Windows, we artificially
      // limit ourselves to 4 GB. But it is probably okay since we don't
      // expect the log contents to be > 4 GB. Fixing would require changing
      // the signature of this function.
      if (pos < ULONG_MAX)
      {
         len = static_cast<unsigned long>(pos);
         *ppContents = new char[len];
         if (0 != *ppContents)
         {
            ifile.seekg(0, ios::beg);
            ifile.read(*ppContents, len);
            ifile.close();
         }
      }
   }

   // re-open for logging
   plogFile_g->open(logFileName_.c_str(), ios_base::app);

   return;
}


std::string FastLogger::LogPath(void)
{
   return logFileName_;

}

