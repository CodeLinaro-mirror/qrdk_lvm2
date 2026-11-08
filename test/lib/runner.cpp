/* -*- C++ -*- copyright (c) 2014-2026 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "brick-shelltest.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <unistd.h>

/*
 * A test suite owns the loop devices and the device-mapper namespace of the
 * host, so two runs of one build tree collide: the second one finds the
 * suspended devices of the first and skips its tests.  Take a lock in the
 * test directory for the whole run and let a second run wait for it.  The
 * descriptor is closed on exec, so test scripts and the daemons they start
 * do not keep the lock after the run.
 */
static void lock_test_run(int argc, const char **argv)
{
    std::string dir = ".";

    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--testdir")) {
            dir = argv[i + 1];
            break;
        }

    std::string path = dir + "/.test-run.lock";
    int fd = open(path.c_str(), O_RDONLY | O_CREAT | O_CLOEXEC, 0666);

    if (fd < 0)
        return;

    if (!flock(fd, LOCK_EX | LOCK_NB) || errno != EWOULDBLOCK)
        return;

    std::cout << "Waiting for another test run of " << dir
              << " to finish..." << std::flush;
    if (flock(fd, LOCK_EX))
        return;
    std::cout << " done." << std::endl;
}

int main(int argc, const char **argv)
{
    lock_test_run(argc, argv);

    try {
        return brick::shelltest::run( argc, argv, "LVM_TEST_FLAVOUR" );
    } catch (std::exception const& e) {
        std::cout << "Exception: " << e.what() << "\n";
    }

    return 1;
}

