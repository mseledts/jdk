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
 * @run driver TestJcmd
 */
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.stream.Collectors;
import jdk.test.lib.Container;
import jdk.test.lib.JDKToolFinder;
import jdk.test.lib.Utils;
import jdk.test.lib.containers.docker.Common;
import jdk.test.lib.containers.docker.DockerRunOptions;
import jdk.test.lib.containers.docker.DockerfileConfig;
import jdk.test.lib.containers.docker.DockerTestUtils;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;


public class TestJcmd {
    private static final String IMAGE_NAME = Common.imageName("jcmd");
    private static final int TIME_TO_RUN_MAIN_PROCESS = (int) (30 * Utils.TIMEOUT_FACTOR); // seconds
    private static final String CONTAINER_NAME = "test-container";

    public static void main(String[] args) throws Exception {
        DockerTestUtils.canTestDocker();

        // Note: userId and groupId of the container should match those of the inspecting process
        String uid = getId("-u");
        String gid = getId("-g");
        String userName = getId("-un");
        String groupName = getId("-gn");
        String content = generateCustomDockerfile(uid, gid, userName, groupName);
        DockerTestUtils.buildJdkDockerImage(IMAGE_NAME, "Dockerfile-Jcmd", "jcmd-docker", content);

        try {

            Process p = startObservedContainer();

            // Need to get PID from the host point of view
            // long pid = getPid("EventGeneratorLoop");
            long pid = testJcmdGetPid();

            Thread.sleep(10*1000);

            assertIsAlive(p);
            testJcmdHelp(pid);

            p.waitFor();
        } finally {
            DockerTestUtils.removeDockerImage(IMAGE_NAME);
        }
    }

    private static String generateCustomDockerfile(String uid, String gid,
                                            String userName, String groupName) throws Exception {
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("FROM %s:%s\n", DockerfileConfig.getBaseImageName(),
                                DockerfileConfig.getBaseImageVersion()));
        sb.append("COPY /jdk /jdk\n");
        sb.append("ENV JAVA_HOME=/jdk\n");

        sb.append(String.format("RUN groupadd --gid %s %s \n", gid, groupName));
        sb.append(String.format("RUN useradd  --uid %s --gid %s %s \n", uid, gid, userName));
        sb.append(String.format("USER %s \n", userName));

        sb.append("CMD [\"/bin/bash\"]\n");

        return sb.toString();
    }


    private static Process startObservedContainer() throws Exception {
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
        return ProcessTools.startProcess("main-container-process",
                                      pb,
                                      line -> line.contains(EventGeneratorLoop.MAIN_METHOD_STARTED),
                                      5, TimeUnit.SECONDS);
    }
    // Run "jcmd -l" in a sidecar container, find a target process.
    private static long testJcmdGetPid() throws Exception {
        System.out.println("testJcmdGetPid()");
        ProcessBuilder pb = new ProcessBuilder(JDKToolFinder.getJDKTool("jcmd"), "-l");
        OutputAnalyzer out = new OutputAnalyzer(pb.start())
            .shouldHaveExitValue(0)
            .shouldContain("sun.tools.jcmd.JCmd");

        System.out.println("------------------ jcmd -l output: ");
        System.out.println(out.getOutput());
        System.out.println("-----------------------------------");

        long pid = findProcess(out, "EventGeneratorLoop");
        if (pid == -1) {
            throw new RuntimeException("Could not find specified process");
        }

        return pid;
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

    private static void testJcmdHelp(long pid) throws Exception {
        System.out.println("testJcmdHelp()");
        ProcessBuilder pb = new ProcessBuilder(JDKToolFinder.getJDKTool("jcmd"), "" + pid, "help");
        System.out.println("testJcmdHelp(): cmd line: " + ProcessTools.getCommandLine(pb));
        OutputAnalyzer out = new OutputAnalyzer(pb.start())
            .shouldHaveExitValue(0)
            .shouldContain("JFR.start")
            .shouldContain("VM.version");
        // TODO: remove
        System.out.println("testJcmdHelp(): ----------------: " + out.getOutput());
    }

    private static void testJcmdVmVersion(long pid) throws Exception {
        // TODO: implement
    }

    private static void assertIsAlive(Process p) throws Exception {
        if (!p.isAlive()) {
            throw new RuntimeException("Main container process stopped unexpectedly, exit value: "
                                       + p.exitValue());
        }
    }

    private static long getPid(String name) throws Exception {
        ProcessBuilder pb = new ProcessBuilder("ps", "-ef");
        OutputAnalyzer out = new OutputAnalyzer(pb.start())
            .shouldHaveExitValue(0);

        List<String> l = out.asLines()
            .stream()
            .filter(s -> s.contains(name))
            .collect(Collectors.toList());

        if (l.isEmpty()) {
            throw new RuntimeException("Could not find process matching " + name);
        }

        String psInfo = l.get(0);
        System.out.println("getPid(): psInfo: " + psInfo);
        String pid = psInfo.split("\\s+", 10)[1];
        System.out.println("getPid(): pid: " + pid);
        return Long.parseLong(pid);
    }

    // -u for userId, -g for groupId
    private static String getId(String param) throws Exception {
        ProcessBuilder pb = new ProcessBuilder("id", param);
        OutputAnalyzer out = new OutputAnalyzer(pb.start())
            .shouldHaveExitValue(0);
        String result = out.asLines().get(0);
        System.out.println("getId() " + param + " returning: " + result);
        return result;
    }
}
