/*
 * Copyright (c) 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */


/*
 * @test
 * @summary Test JCMD across container boundary. The JCMD runs on a host system,
 *          while sending commands to a JVM that runs inside a container.
 * @requires docker.support
 * @requires vm.flagless
 * @modules java.base/jdk.internal.misc
 *          java.management
 *          jdk.jartool/sun.tools.jar
 * @library /test/lib
 * @build EventGeneratorLoop
 * @run driver TestJcmdWithSideCar
 */
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import java.util.stream.Collectors;
import jdk.test.lib.Container;
import jdk.test.lib.Utils;
import jdk.test.lib.containers.docker.Common;
import jdk.test.lib.containers.docker.DockerRunOptions;
import jdk.test.lib.containers.docker.DockerTestUtils;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;


public class TestJcmd {
    private static final String IMAGE_NAME = Common.imageName("jcmd");
    private static final int TIME_TO_RUN_MAIN_PROCESS = (int) (30 * Utils.TIMEOUT_FACTOR); // seconds
    private static final String CONTAINER_NAME = "test-container";

    public static void main(String[] args) throws Exception {
        if (!DockerTestUtils.canTestDocker()) {
            return;
        }

        DockerTestUtils.buildJdkDockerImage(IMAGE_NAME, "Dockerfile-BasicTest", "jdk-docker");

        try {
            Process p = startObservedContainer();

            long pid = testJcmdGetPid();

            assertIsAlive(p);
            testJcmdHelp(pid);

            assertIsAlive(p);
            testJcmdVmInfo(pid);

            p.waitFor();
        } finally {
            DockerTestUtils.removeDockerImage(IMAGE_NAME);
        }
    }


    // Run "jcmd -l" in a sidecar container, find a target process.
    private static long testJcmdGetPid() throws Exception {
        System.out.println("testJcmdGetPid()");
        ProcessBuilder pb = new ProcessBuilder(JDKToolFinder.getJDKTool("jcmd"), "-l");
        OutputAnalyzer out = new OutputAnalyzer(pb.start())
            .shouldHaveExitValue(0)
            .shouldContain("sun.tools.jcmd.JCmd");
        long pid = findProcess(out, "EventGeneratorLoop");
        if (pid == -1) {
            throw new RuntimeException("Could not find specified process");
        }

        return pid;
    }

    private static void testJcmdHelp(long pid) throws Exception {
        System.out.println("testJcmdHelp()");
        ProcessBuilder pb = new ProcessBuilder(JDKToolFinder.getJDKTool("jcmd"), "" + pid, "help");
        OutputAnalyzer out = new OutputAnalyzer(pb.start())
            .shouldHaveExitValue(0)
            .shouldContain("Java System Properties")
            .shouldContain("VM Flags");
    }

    // test some other JCMD command (VM.info, VM.version)
    private static void testJcmdVmInfo(long pid) throws Exception {
        // TODO: implement
    }


    // Returns PID of a matching process, or -1 if not found.
    private static long findProcess(OutputAnalyzer out, String name) throws Exception {
        List<String> l = out.asLines()
            .stream()
            .filter(s -> s.contains(name))
            .collect(Collectors.toList());
        if (l.isEmpty()) {
            return -1;
        }
        String psInfo = l.get(0);
        System.out.println("findProcess(): psInfo: " + psInfo);
        String pid = psInfo.substring(0, psInfo.indexOf(' '));
        System.out.println("findProcess(): pid: " + pid);
        return Long.parseLong(pid);
    }

    public static Process startObservedContainer() throws Exception {
        DockerRunOptions opts = new DockerRunOptions(IMAGE_NAME, "/jdk/bin/java", "EventGeneratorLoop");
        opts.addDockerOpts("--volume", Utils.TEST_CLASSES + ":/test-classes/")
            .addJavaOpts("-cp", "/test-classes/")
            .addDockerOpts("--cap-add=SYS_PTRACE")
            .addDockerOpts("--name", CONTAINER_NAME)
            .addJavaOpts("-XX:+UsePerfData") // TODO: do we really need this one
            .addClassOptions("" + TIME_TO_RUN_MAIN_PROCESS);
        // avoid large Xmx
        opts.appendTestJavaOptions = false;

        List<String> cmd = DockerTestUtils.buildJavaCommand(opts);
        ProcessBuilder pb = new ProcessBuilder(cmd);
        p = ProcessTools.startProcess("main-container-process",
                                      pb,
                                      line -> line.contains(EventGeneratorLoop.MAIN_METHOD_STARTED),
                                      5, TimeUnit.SECONDS);
        return p;
    }

    public static void assertIsAlive(Process p) throws Exception {
        if (!p.isAlive()) {
            throw new RuntimeException("Main container process stopped unexpectedly, exit value: "
                                       + p.exitValue());
        }
    }
}
