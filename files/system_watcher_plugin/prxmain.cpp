#include <vshlib.hpp>
#include <vsh/newDelete.hpp>

#include "Utils/Syscalls.hpp"
#include "Utils/Threads.hpp"
#include "Utils/Timer.hpp"
#include "Utils/Memory/Common.hpp"

#include "system_watcher_plugin.hpp"

#pragma diag_suppress 77
SYS_MODULE_INFO(system_watcher, 0, 1, 1);
SYS_MODULE_START(module_start);
SYS_MODULE_STOP(module_stop);


Thread gModuleStartThread;
bool gRunning = false;
bool gInitialized = false;

extern "C"
{
	int __cdecl module_start(size_t args, const void *argp)
	{
		gModuleStartThread = Thread([]
		{
			do
			{
				Timer::Sleep(1000);
			} while (!paf::View::Find("explore_plugin"));

			gRunning = true;

			while (gRunning)
			{
				xmb_plugin = paf::View::Find("xmb_plugin");
				system_plugin = paf::View::Find("system_plugin");
				page_xmb_indicator = xmb_plugin ? xmb_plugin->FindWidget("page_xmb_indicator") : nullptr;
				page_notification = system_plugin ? system_plugin->FindWidget("page_notification") : nullptr;

				if (!gInitialized && page_xmb_indicator)
				{
					if (LoadIpText())
					{
						Install();
						gInitialized = true;
					}
				}

				if (gInitialized && CanCreateIpText())
					CreateIpText();

				Timer::Sleep(500);
			}

		}, &gModuleStartThread, "module_start()");

		ExitModuleThread();
		return 0;
	}

	int __cdecl module_stop(size_t args, const void *argp)
	{
		Thread moduleStopThread = Thread([]
		{
			gRunning = false;
			if (gInitialized)
				Remove();

			sys_ppu_thread_yield();
			Timer::Sleep(1000);

			gModuleStartThread.Join();

		}, &moduleStopThread, "module_stop()");

		moduleStopThread.Join();

		Timer::Sleep(5);

		UnloadMyModule();
		ExitModuleThread();
		return 0;
	}
}