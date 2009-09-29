/* Copyright (C) 2009 Mobile Sorcery AB

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2, as published by
the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING.  If not, write to the Free
Software Foundation, 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.
*/

#include <stdio.h>
#include <stdarg.h>
#include <iostream>
#include <fstream>
#ifdef _MSC_VER
#include <io.h>
#endif
#include <fcntl.h>

#include "config.h"
#include "helpers/helpers.h"

#include "bluetooth/btinit.h"
#include "sld.h"
#include "stabs/stabs.h"

#include "command.h"
#include "Thread.h"
#include "async.h"
#include "userInputThread.h"
#include "remoteReadThread.h"
#include "StubConnLow.h"
#include "helpers.h"
#include "globals.h"
#include "commandInterface.h"
#include "StubConnection.h"
#include "cmd_stack.h"

using namespace std;

string gProgramFilename, gResourceFilename, gSldFilename;

bool gTestWaiting;

MA_HEAD gHead;
byte* gMemCs = NULL;
int* gMemCp = NULL;

//extern char* sMemBuf;	//TODO: fixme

static string sToken;

static void executeCommand(const string& line);
static bool readProgramFile(const char* filename);

int eprintf(const char* fmt, ...) {
	va_list argptr;
	va_start(argptr, fmt);
#ifdef LOGGING_ENABLED
	LogV(fmt, argptr);
#endif
	return vfprintf(stderr, fmt, argptr);
}

int oprintf(const char* fmt, ...) {
	va_list argptr;
	va_start(argptr, fmt);
#ifdef LOGGING_ENABLED
	LogV(fmt, argptr);
#endif
	return vprintf(fmt, argptr);
}

void oputc(int c) {
#ifdef LOGGING_ENABLED
	LogBin(&c, 1);
#endif
	putc(c, stdout);
}

static MoSyncThread sRemoteReadThread, sUserInputThread;

static const char* sUsageString =
"usage: mdb [options]\n"
"\n"
"The MoSync Debugger exposes a subset of the GDB/MI interface.\n"
"Commands are read from stdin, responses are written to stdout.\n"
"\n"
"It can connect to a remote stub, or start a simulator and connect to that.\n"
"If you're going to use a simulator, you'll need to provide program,\n"
"resource (if the program has any) and SLD files to load.\n"
"\n"
"Options:\n"
"\n"
"-p <program file>\n"
"Loads the specified program file. This argument must always be specified.\n"
"\n"
"-r <resource file>\n"
"Loads the specified resource file, if the simulator target is chosen.\n"
"\n"
"-sld <sld file>\n"
"Loads the specified SLD file.\n"
"Can be specified with the -file-exec-and-symbols command instead of on the\n"
"command line.\n"
"\n"
"An SLD file contains MoSync debugging information.\n"
"It is generated by pipe-tool.\n"
"\n"
"-stabs <stabs file>\n"
"Loads the specified Stabs file.\n"
"\n"
"A Stabs file contains extra debugging information.\n"
"It is generated by pipe-tool.\n"
"\n"
"-s <script file>\n"
"Executes the mdb commands in the script file before reading from stdin.\n"
"\n"
;

#undef main
int main(int argc, char** argv) {
#ifdef LOGGING_ENABLED
	InitLog("mdb_log.txt");
#endif
	for(int i=0; i<argc; i++) {
		LOG("%s ", argv[i]);
	}
	LOG("\n");

	istream* input = &cin;
	const char* stabsFilename = NULL;

	//parse arguments
	int i = 1;
	while(i < argc) {
		if(strcmp(argv[i], "-s") == 0) {
			i++;
			if(i > argc) {
				eprintf("%s", sUsageString);
				return 1;
			}
			input = new ifstream(argv[i]);
			if(!input->good()) {
				eprintf("Could not open script file '%s'\n", argv[2]);
				return 1;
			}
		} else if(strcmp(argv[i], "-p") == 0) {
			i++;
			if(i > argc) {
				eprintf("%s", sUsageString);
				return 1;
			}
			gProgramFilename = argv[i];
		} else if(strcmp(argv[i], "-r") == 0) {
			i++;
			if(i > argc) {
				eprintf("%s", sUsageString);
				return 1;
			}
			gResourceFilename = argv[i];
		} else if(strcmp(argv[i], "-sld") == 0) {
			i++;
			if(i > argc) {
				eprintf("%s", sUsageString);
				return 1;
			}
			gSldFilename = argv[i];
		} else if(strcmp(argv[i], "-stabs") == 0) {
			i++;
			if(i > argc) {
				eprintf("%s", sUsageString);
				return 1;
			}
			stabsFilename = argv[i];
		} else {
			eprintf("%s", sUsageString);
			return 1;
		}
		i++;
	}

	if(gProgramFilename.size() > 0) {
		if(!readProgramFile(gProgramFilename.c_str())) {
			eprintf("Could not load program file '%s'\n", gProgramFilename.c_str());
			return 1;
		}
	} else {
		eprintf("Must specify a program file!\n");
		return 1;
	}

	if(stabsFilename) {
		//load stabs and SLD
		if(!loadStabs(gSldFilename.c_str(), stabsFilename)) {
			eprintf("Could not load Stabs file '%s'\n", stabsFilename);
			return 1;
		}
	} else if(gSldFilename.size() > 0) {
		//if no stabs, load only SLD.
		if(!loadSLD(gSldFilename.c_str())) {
			eprintf("Could not load SLD file '%s'\n", gSldFilename.c_str());
			return 1;
		}
	}

	//initialize subsystems
	Bluetooth::MABtInit();
	initCommands();
	initEventSystem();

	StubConnection::addContinueListener(stackContinued);

	sUserInputThread.start(userInputThreadFunc, input);
	sRemoteReadThread.start(remoteReadThreadFunc, NULL);

	oprintf(GDB_PROMPT);	//undocumented startup prompt, expected by ccdebug.
	fflush(stdout);

	//main loop
	LOG("Starting main loop...\n");
	string savedLine;
	while(1) {
		DebuggerEvent* de;
		getEvent(&de);
		//LOG("Event %i\n", de->type);
		switch(de->type) {
		case DebuggerEvent::eRecv:
			StubConnLow::recvHandler(de->result);
			break;
		case DebuggerEvent::eUserInput:
			if(StubConnection::isIdle())
				executeCommand(de->str);
			else
				savedLine = de->str;
			break;
		case DebuggerEvent::eReadMemory:
			StubConnection::readMemory(gMemBuf + de->src, de->src, de->len, de->rmcb);
			break;
		case DebuggerEvent::eLocateSymbol:
			locate_symbol(de->str, de->lscb);
			break;
		case DebuggerEvent::eExpressionEvaluated:
			setErrorCallback(NULL);
			de->ecb(de->erv, de->err);
			break;
		default:
			_ASSERT(false);
		}
		delete de;
		if(StubConnection::isIdle() && savedLine.size() > 0) {
			executeCommand(savedLine);
			savedLine.clear();
		}
		if(gTestWaiting && !StubConnection::isRunning()) {
			gTestWaiting = false;
			oprintDone();
			oprintf(",test-wait\n");
			commandComplete();
		}
	}
}

void oprintToken() {
	oprintf("%s", sToken.c_str());
}
void oprintDone() {
	oprintf("%s^done", sToken.c_str());
}
void oprintDoneLn() {
	oprintf("%s^done\n", sToken.c_str());
}

static ErrorCallback sErrorCallback = NULL;
void setErrorCallback(ErrorCallback ecb) {
	sErrorCallback = ecb;
}

void error(const char* fmt, ...) {
	if(sErrorCallback) sErrorCallback();
	va_list argptr;
	va_start(argptr, fmt);
	oprintf("%s^error,msg=\"", sToken.c_str());
#ifdef LOGGING_ENABLED
	LogV(fmt, argptr);
#endif
	vprintf(fmt, argptr);
	oprintf("\"\n");
	commandComplete();
}

void commandComplete() {
	oprintf(GDB_PROMPT);
	fflush(stdout);

	//user input is automatically paused after one line is read,
	//so now that we've processed it, we can resume.
	resumeUserInput();
}

static void executeCommand(const string& line) {
	LOG("Command: %s\n", line.c_str());
	size_t offset = 0;
	if(isdigit(line[0])) {	//we've got a token
		offset = line.find('-');
		if(offset == string::npos) {
			//special handling for Eclipse' broken use of "whatis"
			offset = line.find(' ');
			if(offset == string::npos) {
				error("Command syntax error");
				return;
			}
		}
		sToken = line.substr(0, offset);
	} else {
		sToken.clear();
	}
	size_t firstSpaceIndex = line.find_first_of(' ', offset + 1);
	string command = line.substr(offset, firstSpaceIndex - offset);
	CommandIterator itr = sCommands.find(command);
	if(itr != sCommands.end()) {
		Command cmd = itr->second;
		int argIndex = line.find_first_not_of(' ', firstSpaceIndex);
		string args = argIndex > 0 ? line.substr(argIndex) : "";
		cmd(args);
	} else {
		error("Undefined MI command: '%s'", line.c_str());
		commandComplete();
	}
}

void MoSyncErrorExit(int code) {
	error("MoSyncErrorExit(%i)", code);
	exit(code);
}

static bool readOpenProgramFile(int fd) {
	MA_HEAD& h(gHead);
	int res = read(fd, &h, sizeof(h));
	TEST(res == sizeof(h));

	if(h.Magic != 0x5844414d) {	//MADX, big-endian
		LOG("Magic error: 0x%08x should be 0x5844414d\n", h.Magic);
		FAIL;
	}

	//ok, we read it. now apply...
	setMemSize(h.DataSize);

	gMemCs = new byte[h.CodeLen];
	res = read(fd, gMemCs, h.CodeLen);
	TEST(res == h.CodeLen);

	if(lseek(fd, h.DataLen, SEEK_CUR) < 0) {
		FAIL;
	}

	gMemCp = new int[h.IntLen];
	res = read(fd, gMemCp, h.IntLen * 4);
	TEST(res == h.IntLen * 4);
	return true;
}

static bool readProgramFile(const char* filename) {
	int fd = open(filename, O_RDONLY
#ifdef _MSC_VER
		| O_BINARY
#endif
		);
	FAILIF(fd < 0);

	bool res = readOpenProgramFile(fd);

	close(fd);

	return res;
}
