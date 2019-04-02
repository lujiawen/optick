#pragma once
#if defined(__APPLE_CC__)

#include "Platform_Common.h"

#include <mach/mach_time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>

#include "Trace.h"

namespace Optick
{
	const char* Platform::GetName()
	{
		return "MacOS";
	}

	ThreadID Platform::GetThreadID()
	{
		uint64_t tid;
		pthread_threadid_np(pthread_self(), &tid);
		return tid;
	}

	ProcessID Platform::GetProcessID()
	{
		return (ProcessID)getpid();
	}

	int64 Platform::GetFrequency()
	{
		return 1000000000;
	}

	int64 Platform::GetTime()
	{
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		return ts.tv_sec * 1000000000LL + ts.tv_nsec;
	}
}

#if OPTICK_ENABLE_TRACING

#include <array>
#include <vector>
#include "Core.h"

namespace Optick
{
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
class DTrace : public Trace
{
	static const bool isSilent = true;

	std::thread processThread;
	std::string password;

	enum State
	{
		STATE_IDLE,
		STATE_RUNNING,
		STATE_ABORT,
	};

	volatile State state;
	volatile int64 timeout;

	struct CoreState
	{
		ProcessID pid;
		ThreadID tid;
		int prio;
		bool IsValid() const { return tid != INVALID_THREAD_ID; }
		CoreState() : pid(INVALID_THREAD_ID), tid(INVALID_THREAD_ID), prio(0) {}
	};
	static const int MAX_CPU_CORES = 256;
	std::array<CoreState, MAX_CPU_CORES> cores;

	static void AsyncProcess(DTrace* trace);
	void Process();

	bool CheckRootAccess();

	enum ParseResult
	{
		PARSE_OK,
		PARSE_TIMEOUT,
		PARSE_FAILED,
	};
	ParseResult Parse(const char* line);
public:

	DTrace();

	virtual CaptureStatus::Type Start(Trace::Mode mode, const ThreadList& threads) override;
	virtual bool Stop() override;
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
DTrace g_DTrace;
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
DTrace::DTrace() : state(STATE_IDLE), timeout(0)
{
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DTrace::CheckRootAccess()
{
	char cmd[256] = { 0 };
	sprintf_s(cmd, "echo \'%s\' | sudo -S echo %s", password.c_str(), isSilent ? "2> /dev/null" : "");
	return system(cmd) == 0;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CaptureStatus::Type DTrace::Start(Trace::Mode mode, const ThreadList& threads)
{
	if (state == STATE_IDLE)
	{
		if (!CheckRootAccess())
			return CaptureStatus::ERR_TRACER_INVALID_PASSWORD;

		state = STATE_RUNNING;
		timeout = INT64_MAX;
		cores.fill(CoreState());
		processThread = std::thread(AsyncProcess, this);
	}

	return CaptureStatus::OK;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool DTrace::Stop()
{
	if (state != STATE_RUNNING)
	{
		return false;
	}

	timeout = Platform::GetTime();
	processThread.join();
	state = STATE_IDLE;

	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FILE* popen2(const char *program, const char *type, pid_t* outPid)
{
	FILE *iop;
	int pdes[2];
	pid_t pid;
	if ((*type != 'r' && *type != 'w') || type[1] != '\0') {
		errno = EINVAL;
		return (NULL);
	}

	if (pipe(pdes) < 0) {
		return (NULL);
	}

	switch (pid = fork()) {
	case -1:            /* Error. */
		(void)close(pdes[0]);
		(void)close(pdes[1]);
		return (NULL);
		/* NOTREACHED */
	case 0:                /* Child. */
	{
		if (*type == 'r') {
			(void)close(pdes[0]);
			if (pdes[1] != STDOUT_FILENO) {
				(void)dup2(pdes[1], STDOUT_FILENO);
				(void)close(pdes[1]);
			}
		}
		else {
			(void)close(pdes[1]);
			if (pdes[0] != STDIN_FILENO) {
				(void)dup2(pdes[0], STDIN_FILENO);
				(void)close(pdes[0]);
			}
		}
		execl("/bin/sh", "sh", "-c", program, NULL);
		perror("execl");
		exit(1);
		/* NOTREACHED */
	}
	}
	/* Parent; assume fdopen can't fail. */
	if (*type == 'r') {
		iop = fdopen(pdes[0], type);
		(void)close(pdes[1]);
	}
	else {
		iop = fdopen(pdes[1], type);
		(void)close(pdes[0]);
	}

	if (outPid)
		*outPid = pid;

	return (iop);
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void DTrace::Process()
{
	const char* command = "dtrace -n fbt::thread_dispatch:return'\\''{printf(\"@%d %d %d %d\", pid, tid, curthread->sched_pri, walltimestamp)}'\\''";

	char buffer[256] = { 0 };
	sprintf_s(buffer, "echo \'%s\' | sudo -S sh -c \'%s\' %s", password.c_str(), command, isSilent ? "2> /dev/null" : "");
	pid_t pid;
	if (FILE* pipe = popen2(buffer, "r", &pid))
	{
		char* line = NULL;
		size_t len = 0;
		while (state == STATE_RUNNING && (getline(&line, &len, pipe)) != -1)
		{
			if (Parse(line) == PARSE_TIMEOUT)
				break;
		}
		fclose(pipe);

		int internal_stat;
		waitpid(pid, &internal_stat, 0);
	}
	else
	{
		OPTICK_FAILED("Failed to open communication pipe!");
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
DTrace::ParseResult DTrace::Parse(const char* line)
{
	if (const char* cmd = strchr(line, '@'))
	{
		int cpu = atoi(line);

		CoreState currState;

		currState.pid = atoi(cmd + 1);
		cmd = strchr(cmd, ' ') + 1;

		currState.tid = atoi(cmd);
		cmd = strchr(cmd, ' ') + 1;

		currState.prio = atoi(cmd);
		cmd = strchr(cmd, ' ') + 1;

		uint64_t timestamp = atoll(cmd);

		if (timestamp > timeout)
			return PARSE_TIMEOUT;

		const CoreState& prevState = cores[cpu];

		if (prevState.IsValid())
		{
			Optick::SwitchContextDesc desc;
			desc.reason = 0;
			desc.cpuId = cpu;
			desc.oldThreadId = prevState.tid;
			desc.newThreadId = currState.tid;
			desc.timestamp = timestamp;
			Core::Get().ReportSwitchContext(desc);
		}

		cores[cpu] = currState;
	}
	return PARSE_FAILED;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void DTrace::AsyncProcess(Optick::DTrace *trace) {
	trace->Process();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
Trace* Platform::GetTrace()
{
	return &g_DTrace;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
}
#endif //OPTICK_ENABLE_TRACING

#endif