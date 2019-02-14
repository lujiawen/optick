#if defined(__linux__)

#include "Trace.h"

#if BRO_ENABLE_TRACING

#include <array>
#include <vector>
#include <string>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
namespace ft
{
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct base_event
{
	int64_t timestamp;
	short common_type;
	uint8_t cpu_id;
	base_event(short type) : timestamp(-1), common_type(type), cpu_id(uint8_t(-1)) {}
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template<short TYPE>
struct event : public base_event
{
	static const short type = TYPE;
	event() : base_event(TYPE) {}
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct process_state
{
	enum type
	{
		Unknown,
		//D	Uninterruptible sleep(usually IO)
		UninterruptibleSleep,
		//R	Running or runnable(on run queue)
		Running,
		//S	Interruptible sleep(waiting for an event to complete)
		InterruptibleSleep,
		//T	Stopped, either by a job control signal or because it is being traced.
		Stopped,
		//X	dead(should never be seen)
		Dead,
		//Z	Defunct(�zombie�) process, terminated but not reaped by its parent.
		Zombie,
	};
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct sched_switch : public event<305>
{
	char prev_comm[16];
	pid_t prev_pid;
	int prev_prio;
	process_state::type prev_state;
	char next_comm[16];
	pid_t next_pid;
	int next_prio;
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
} // namespace ft
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace Brofiler
{
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static const char* KERNEL_TRACING_PATH = "/sys/kernel/debug/tracing";
static const char* FTRACE_TRACE = "trace";
static const char* FTRACE_TRACING_ON = "tracing_on";
static const char* FTRACE_TRACE_CLOCK = "trace_clock";
static const char* FTRACE_OPTIONS_IRQ_INFO = "options/irq-info";
static const char* FTRACE_SCHED_SWITCH = "events/sched/sched_switch/enable";
static const uint8_t PROCESS_STATE_REASON_START = 40;
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
class FTrace : public Trace
{
	bool isActive;
	std::string password;

	bool Parse(const char* line);
	bool ProcessEvent(const ft::base_event& ev);

	void Set(const char* name, bool value);
	void Set(const char* name, const char* value);
	void Exec(const char* cmd);
public:

	FTrace();
	~FTrace();

	virtual CaptureStatus::Type Start(int mode, const ThreadList& threads) override;
	virtual bool Stop() override;
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FTrace g_FTrace;
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
struct Parser
{
	const char* cursor;
	const char* finish;
	size_t length;

	Parser(const char* b) : cursor(b), finish(b + strlen(b)) {}
	
	bool Skip(size_t count)
	{
		if (finish - cursor > count)
		{
			cursor += count;
			return true;
		}
		return false;
	}

	bool Skip(const char* text, char* output = nullptr, size_t size = 0)
	{
		if (const char* ptr = strstr(cursor, text))
		{
			if (output != nullptr)
			{
				size_t count = std::min(size - 1, (size_t)(ptr - cursor));
				strncpy(output, cursor, count);
				output[count] = '\0';
			}
			cursor = ptr + strlen(text);
			return true;
		}
		return false;
	}

	void SkipSpaces()
	{
		while (cursor != finish && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n'))
			++cursor;
	}

	bool Starts(const char* text) const
	{
		return strncmp(cursor, text, sizeof(text)) == 0;
	}

	int GetInt() const
	{
		return atoi(cursor);
	}

	char GetChar() const
	{
		return *cursor;
	}
};
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CaptureStatus::Type FTrace::Start(int mode, const ThreadList& threads)
{
	if (!isActive) 
	{
		// Disable tracing
		Set(FTRACE_TRACING_ON, false);
		// Cleanup old data
		Set(FTRACE_TRACE, "");
		// Set clock type
		Set(FTRACE_TRACE_CLOCK, "mono");
		// Disable irq info
		Set(FTRACE_OPTIONS_IRQ_INFO, false);
		// Enable switch events
		Set(FTRACE_SCHED_SWITCH, (mode & SWITCH_CONTEXTS) != 0);

		// Enable tracing
		Set(FTRACE_TRACING_ON, true);

		isActive = true;
	}

	return CaptureStatus::OK;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool FTrace::Stop()
{
	if (!isActive)
	{
		return false;
	}

	// Reset variables
	Set(FTRACE_TRACING_ON, false);
	Set(FTRACE_SCHED_SWITCH, false);

	// Parsing the output
	char buffer[256] = { 0 };
	sprintf_s(buffer, "echo \'%s\' | sudo -S sh -c \'cat %s/%s\'", password.c_str(), KERNEL_TRACING_PATH, FTRACE_TRACE);
	if (FILE* pipe = popen(buffer, "r"))
	{
		char* line = NULL;
		size_t len = 0;
		while ((getline(&line, &len, pipe)) != -1)
			Parse(line);
		fclose(pipe);
	}

	// Cleanup data
	Set(FTRACE_TRACE, "");

	isActive = false;

	if (!Trace::Stop())
	{
		return false;
	}

	return true;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool FTrace::Parse(const char * line)
{
	// sched_switch:      
	//	   ConsoleApp-8687  [000]  181944.352057: sched_switch: prev_comm=ConsoleApp prev_pid=8687 prev_prio=120 prev_state=S ==> next_comm=ConsoleApp next_pid=8686 next_prio=120

	Parser p(line);
	if (p.Starts("#"))
		return true;

	if (!p.Skip(16))
		return false;

	if (!p.Skip("["))
		return false;

	int cpu = p.GetInt();
	if (!p.Skip("]"))
		return false;

	int64 timestampInt = p.GetInt();
	if (!p.Skip("."))
		return false;

	int64 timestampFraq = p.GetInt();
	if (!p.Skip(": "))
		return false;

	int64 timestamp = ((timestampInt * 1000000) + timestampFraq) * 1000;

	if (p.Starts("sched_switch:"))
	{
		ft::sched_switch ev;
		ev.cpu_id = cpu;
		ev.timestamp = timestamp;

		if (!p.Skip("prev_comm="))
			return false;

		if (!p.Skip(" prev_pid=", ev.prev_comm, BRO_ARRAY_SIZE(ev.prev_comm)))
			return false;

		ev.prev_pid = p.GetInt();

		if (!p.Skip(" prev_prio="))
			return false;

		ev.prev_prio = p.GetInt();

		if (!p.Skip(" prev_state="))
			return false;

		switch (p.GetChar())
		{
		case 'D':
			ev.prev_state = ft::process_state::UninterruptibleSleep;
			break;

		case 'R':
			ev.prev_state = ft::process_state::Running;
			break;

		case 'S':
			ev.prev_state = ft::process_state::InterruptibleSleep;
			break;

		case 'T':
			ev.prev_state = ft::process_state::Stopped;
			break;

		case 'X':
			ev.prev_state = ft::process_state::Dead;
			break;

		case 'Z':
			ev.prev_state = ft::process_state::Zombie;
			break;

		default:
			ev.prev_state = ft::process_state::Unknown;
			break;
		}

		if (!p.Skip("==> next_comm="))
			return false;

		if (!p.Skip(" next_pid=", ev.next_comm, BRO_ARRAY_SIZE(ev.prev_comm)))
			return false;

		ev.next_pid = p.GetInt();

		if (!p.Skip(" next_prio="))
			return false;

		ev.next_prio = p.GetInt();

		return ProcessEvent(ev);
	}

}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool FTrace::ProcessEvent(const ft::base_event& ev)
{
	switch (ev.common_type)
	{
	case ft::sched_switch::type:
		{
			const ft::sched_switch& switchEv = (const ft::sched_switch&)ev;
			Brofiler::SwitchContextDesc desc;
			desc.reason = switchEv.prev_state + PROCESS_STATE_REASON_START;
			desc.cpuId = switchEv.cpu_id;
			desc.oldThreadId = (uint64)switchEv.prev_pid;
			desc.newThreadId = (uint64)switchEv.next_pid;
			desc.timestamp = switchEv.timestamp;
			Core::Get().ReportSwitchContext(desc);
			return true;
		}
		break;
	}

	return false;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void FTrace::Set(const char * name, bool value)
{
	Set(name, value ? "1" : "0");
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void FTrace::Set(const char* name, const char* value)
{
	char buffer[256] = { 0 };
	sprintf_s(buffer, "echo %s > %s/%s", value, KERNEL_TRACING_PATH, name);
	Exec(buffer);
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void FTrace::Exec(const char* cmd)
{
	char buffer[256] = { 0 };
	sprintf_s(buffer, "echo \'%s\' | sudo -S sh -c \'%s\'", password.c_str(), cmd);
	std::system(buffer);
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FTrace::FTrace() : isActive(false)
{
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
FTrace::~FTrace()
{
	Stop();
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
Trace* Trace::Get()
{
	return &g_FTrace;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
}
#endif //BRO_ENABLE_TRACING
#endif //__linux__