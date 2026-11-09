/*
 * crash.c: linked into a test program, says where it crashed (signal, address, and the stack as
 * image + offset, to look up in the unstripped build) instead of a bare "killed by signal 11".
 */
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ucontext.h>
#include <unistd.h>

static void
where(const char *what, uintptr_t address)
{
	Dl_info d;

	if (dladdr((void *)address, &d) && d.dli_fname != NULL) {
		fprintf(stderr, "crash:   %s %#lx = %s + %#lx\n", what, (unsigned long)address, d.dli_fname,
		    (unsigned long)(address - (uintptr_t)d.dli_fbase));
	} else {
		fprintf(stderr, "crash:   %s %#lx\n", what, (unsigned long)address);
	}
}

static void
caught(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	void *frames[32];
	int n = backtrace(frames, 32);

	fprintf(stderr, "crash: signal %d touching %p\n", sig, info->si_addr);
	where("pc", (uintptr_t)uc->uc_mcontext->__ss.__pc);
	where("lr", (uintptr_t)uc->uc_mcontext->__ss.__lr);
	for (int i = 0; i < n; i++) {
		where("frame", (uintptr_t)frames[i]);
	}
	_exit(128 + sig);
}

__attribute__((constructor)) static void
install(void)
{
	struct sigaction sa = { 0 };

	sa.sa_sigaction = caught;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
}
