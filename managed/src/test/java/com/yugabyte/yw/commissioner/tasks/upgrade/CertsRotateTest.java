// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks.upgrade;

import static com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.ServerType.MASTER;
import static com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.ServerType.TSERVER;
import static com.yugabyte.yw.models.TaskInfo.State.Failure;
import static com.yugabyte.yw.models.TaskInfo.State.Success;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.fail;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;

import com.google.common.collect.ImmutableList;
import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.commissioner.tasks.UniverseDefinitionTaskBase.ServerType;
import com.yugabyte.yw.common.CertificateHelper;
import com.yugabyte.yw.common.TestHelper;
import com.yugabyte.yw.forms.CertsRotateParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.forms.UpgradeTaskParams.UpgradeOption;
import com.yugabyte.yw.models.CertificateInfo;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.PlacementInfo;
import com.yugabyte.yw.models.helpers.TaskType;
import java.io.IOException;
import java.security.NoSuchAlgorithmException;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.stream.Collectors;
import junitparams.JUnitParamsRunner;
import junitparams.Parameters;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.InjectMocks;
import org.mockito.MockitoAnnotations;
import org.mockito.junit.MockitoJUnit;
import org.mockito.junit.MockitoRule;

@RunWith(JUnitParamsRunner.class)
public class CertsRotateTest extends UpgradeTaskTest {

  @Rule public MockitoRule rule = MockitoJUnit.rule();

  @InjectMocks private CertsRotate certsRotate;

  private static final List<TaskType> ROLLING_UPGRADE_TASK_SEQUENCE =
      ImmutableList.of(
          TaskType.SetNodeState,
          TaskType.AnsibleClusterServerCtl,
          TaskType.AnsibleClusterServerCtl,
          TaskType.WaitForServer,
          TaskType.WaitForServerReady,
          TaskType.WaitForEncryptionKeyInMemory,
          TaskType.SetNodeState);

  private static final List<TaskType> NON_ROLLING_UPGRADE_TASK_SEQUENCE =
      ImmutableList.of(
          TaskType.SetNodeState,
          TaskType.AnsibleClusterServerCtl,
          TaskType.AnsibleClusterServerCtl,
          TaskType.SetNodeState,
          TaskType.WaitForServer);

  @Override
  @Before
  public void setUp() {
    super.setUp();
    MockitoAnnotations.initMocks(this);
    certsRotate.setUserTaskUUID(UUID.randomUUID());
  }

  private TaskInfo submitTask(CertsRotateParams requestParams) {
    return submitTask(requestParams, TaskType.CertsRotate, commissioner);
  }

  private int assertSequence(
      Map<Integer, List<TaskInfo>> subTasksByPosition,
      ServerType serverType,
      int startPosition,
      boolean isRollingUpgrade) {
    int position = startPosition;
    if (isRollingUpgrade) {
      List<TaskType> taskSequence = ROLLING_UPGRADE_TASK_SEQUENCE;
      List<Integer> nodeOrder = getRollingUpgradeNodeOrder(serverType);
      for (int nodeIdx : nodeOrder) {
        String nodeName = String.format("host-n%d", nodeIdx);
        for (TaskType type : taskSequence) {
          List<TaskInfo> tasks = subTasksByPosition.get(position);
          TaskType taskType = tasks.get(0).getTaskType();

          assertEquals(1, tasks.size());
          assertEquals(type, taskType);
          if (!NON_NODE_TASKS.contains(taskType)) {
            Map<String, Object> assertValues =
                new HashMap<>(ImmutableMap.of("nodeName", nodeName, "nodeCount", 1));
            assertNodeSubTask(tasks, assertValues);
          }
          position++;
        }
      }
    } else {
      for (TaskType type : NON_ROLLING_UPGRADE_TASK_SEQUENCE) {
        List<TaskInfo> tasks = subTasksByPosition.get(position);
        TaskType taskType = assertTaskType(tasks, type);

        if (NON_NODE_TASKS.contains(taskType)) {
          assertEquals(1, tasks.size());
        } else {
          Map<String, Object> assertValues =
              new HashMap<>(
                  ImmutableMap.of(
                      "nodeNames",
                      (Object) ImmutableList.of("host-n1", "host-n2", "host-n3"),
                      "nodeCount",
                      3));
          assertEquals(3, tasks.size());
          assertNodeSubTask(tasks, assertValues);
        }
        position++;
      }
    }
    return position;
  }

  private int assertCommonTasks(
      Map<Integer, List<TaskInfo>> subTasksByPosition,
      int position,
      boolean isRootCAUpdate,
      boolean isUniverseUpdate) {
    if (isRootCAUpdate || isUniverseUpdate) {
      if (isRootCAUpdate) {
        assertTaskType(subTasksByPosition.get(position++), TaskType.UniverseUpdateRootCert);
      }
      if (isUniverseUpdate) {
        assertTaskType(subTasksByPosition.get(position++), TaskType.UniverseSetTlsParams);
      }
    } else {
      List<TaskInfo> certUpdateTasks = subTasksByPosition.get(position++);
      assertTaskType(certUpdateTasks, TaskType.AnsibleConfigureServers);
      assertEquals(3, certUpdateTasks.size());
    }
    return position;
  }

  private int assertRestartSequence(
      Map<Integer, List<TaskInfo>> subTasksByPosition, int position, boolean isRollingUpgrade) {
    if (isRollingUpgrade) {
      position = assertSequence(subTasksByPosition, MASTER, position, true);
      assertTaskType(subTasksByPosition.get(position++), TaskType.LoadBalancerStateChange);
      position = assertSequence(subTasksByPosition, TSERVER, position, true);
      assertTaskType(subTasksByPosition.get(position++), TaskType.LoadBalancerStateChange);
    } else {
      position = assertSequence(subTasksByPosition, MASTER, position, false);
      position = assertSequence(subTasksByPosition, TSERVER, position, false);
    }
    return position;
  }

  private void assertUniverseDetails(
      CertsRotateParams taskParams,
      UUID rootCA,
      UUID clientRootCA,
      boolean currentNodeToNode,
      boolean currentClientToNode,
      boolean rootAndClientRootCASame,
      boolean rotateRootCA,
      boolean rotateClientRootCA,
      boolean isRootCARequired,
      boolean isClientRootCARequired) {
    Universe universe = Universe.getOrBadRequest(defaultUniverse.getUniverseUUID());
    UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
    UserIntent userIntent = universeDetails.getPrimaryCluster().userIntent;
    if (isRootCARequired) {
      if (rotateRootCA) {
        assertEquals(taskParams.rootCA, universeDetails.rootCA);
      } else {
        assertEquals(rootCA, universeDetails.rootCA);
      }
    } else {
      assertNull(universeDetails.rootCA);
    }
    if (isClientRootCARequired) {
      if (rotateClientRootCA) {
        assertEquals(taskParams.clientRootCA, universeDetails.clientRootCA);
      } else {
        assertEquals(clientRootCA, universeDetails.clientRootCA);
      }
    } else {
      assertNull(universeDetails.clientRootCA);
    }
    assertEquals(rootAndClientRootCASame, universeDetails.rootAndClientRootCASame);
    assertEquals(currentNodeToNode, userIntent.enableNodeToNodeEncrypt);
    assertEquals(currentClientToNode, userIntent.enableClientToNodeEncrypt);
  }

  private void prepareUniverse(
      boolean nodeToNode,
      boolean clientToNode,
      boolean rootAndClientRootCASame,
      UUID rootCA,
      UUID clientRootCA)
      throws IOException, NoSuchAlgorithmException {
    CertificateInfo.create(
        rootCA,
        defaultCustomer.uuid,
        "test1",
        new Date(),
        new Date(),
        "privateKey",
        TestHelper.TMP_PATH + "/ca.crt",
        CertificateInfo.Type.SelfSigned);

    CertificateInfo.create(
        clientRootCA,
        defaultCustomer.uuid,
        "test1",
        new Date(),
        new Date(),
        "privateKey",
        TestHelper.TMP_PATH + "/ca.crt",
        CertificateInfo.Type.SelfSigned);

    defaultUniverse =
        Universe.saveDetails(
            defaultUniverse.universeUUID,
            universe -> {
              UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
              PlacementInfo placementInfo = universeDetails.getPrimaryCluster().placementInfo;
              UserIntent userIntent = universeDetails.getPrimaryCluster().userIntent;
              userIntent.enableNodeToNodeEncrypt = nodeToNode;
              userIntent.enableClientToNodeEncrypt = clientToNode;
              universeDetails.allowInsecure = true;
              universeDetails.rootAndClientRootCASame = rootAndClientRootCASame;
              universeDetails.rootCA = null;
              if (CertificateHelper.isRootCARequired(
                  nodeToNode, clientToNode, rootAndClientRootCASame)) {
                universeDetails.rootCA = rootCA;
              }
              universeDetails.clientRootCA = null;
              if (CertificateHelper.isClientRootCARequired(
                  nodeToNode, clientToNode, rootAndClientRootCASame)) {
                universeDetails.clientRootCA = clientRootCA;
              }
              if (nodeToNode || clientToNode) {
                universeDetails.allowInsecure = false;
              }
              universeDetails.upsertPrimaryCluster(userIntent, placementInfo);
              universe.setUniverseDetails(universeDetails);
            },
            false);
  }

  private CertsRotateParams getTaskParams(
      boolean rotateRootCA,
      boolean rotateClientRootCA,
      boolean rootAndClientRootCASame,
      UpgradeOption upgradeOption)
      throws IOException, NoSuchAlgorithmException {
    CertsRotateParams taskParams = new CertsRotateParams();
    taskParams.upgradeOption = upgradeOption;
    if (rotateRootCA) {
      taskParams.rootCA = UUID.randomUUID();
      CertificateInfo.create(
          taskParams.rootCA,
          defaultCustomer.uuid,
          "test1",
          new Date(),
          new Date(),
          "privateKey",
          TestHelper.TMP_PATH + "/ca.crt",
          CertificateInfo.Type.SelfSigned);
    }
    if (rotateClientRootCA) {
      taskParams.clientRootCA = UUID.randomUUID();
      CertificateInfo.create(
          taskParams.clientRootCA,
          defaultCustomer.uuid,
          "test1",
          new Date(),
          new Date(),
          "privateKey",
          TestHelper.TMP_PATH + "/ca.crt",
          CertificateInfo.Type.SelfSigned);
    }
    if (rotateRootCA && rotateClientRootCA && rootAndClientRootCASame) {
      taskParams.clientRootCA = taskParams.rootCA;
    }
    taskParams.rootAndClientRootCASame = rootAndClientRootCASame;

    return taskParams;
  }

  @Test
  public void testCertsRotateNonRestartUpgrade() throws IOException, NoSuchAlgorithmException {
    CertsRotateParams taskParams =
        getTaskParams(false, false, false, UpgradeOption.NON_RESTART_UPGRADE);
    TaskInfo taskInfo = submitTask(taskParams);
    if (taskInfo == null) {
      fail();
    }

    assertEquals(Failure, taskInfo.getTaskState());
    assertEquals(0, taskInfo.getSubTasks().size());
    verify(mockNodeManager, times(0)).nodeCommand(any(), any());
  }

  @Test
  @Parameters({
    "false, false, false, false, false, false",
    "false, false, false, false, false, true",
    "false, false, false, false, true, false",
    "false, false, false, false, true, true",
    "false, false, false, true, false, false",
    "false, false, false, true, false, true",
    "false, false, false, true, true, false",
    "false, false, false, true, true, true",
    "false, false, true, false, false, false",
    "false, false, true, false, false, true",
    "false, false, true, false, true, false",
    "false, false, true, false, true, true",
    "false, false, true, true, false, false",
    "false, false, true, true, false, true",
    "false, false, true, true, true, false",
    "false, false, true, true, true, true",
    "false, true, false, false, false, false",
    "false, true, false, false, false, true",
    "false, true, false, false, true, false",
    "false, true, false, false, true, true",
    "false, true, false, true, false, false",
    "false, true, false, true, false, true",
    "false, true, false, true, true, false",
    "false, true, false, true, true, true",
    "false, true, true, false, false, false",
    "false, true, true, false, false, true",
    "false, true, true, false, true, false",
    "false, true, true, false, true, true",
    "false, true, true, true, false, false",
    "false, true, true, true, false, true",
    "false, true, true, true, true, false",
    "false, true, true, true, true, true",
    "true, false, false, false, false, false",
    "true, false, false, false, false, true",
    "true, false, false, false, true, false",
    "true, false, false, false, true, true",
    "true, false, false, true, false, false",
    "true, false, false, true, false, true",
    "true, false, false, true, true, false",
    "true, false, false, true, true, true",
    "true, false, true, false, false, false",
    "true, false, true, false, false, true",
    "true, false, true, false, true, false",
    "true, false, true, false, true, true",
    "true, false, true, true, false, false",
    "true, false, true, true, false, true",
    "true, false, true, true, true, false",
    "true, false, true, true, true, true",
    "true, true, false, false, false, false",
    "true, true, false, false, false, true",
    "true, true, false, false, true, false",
    "true, true, false, false, true, true",
    "true, true, false, true, false, false",
    "true, true, false, true, false, true",
    "true, true, false, true, true, false",
    "true, true, false, true, true, true",
    "true, true, true, false, false, false",
    "true, true, true, false, false, true",
    "true, true, true, false, true, false",
    "true, true, true, false, true, true",
    "true, true, true, true, false, false",
    "true, true, true, true, false, true",
    "true, true, true, true, true, false",
    "true, true, true, true, true, true",
  })
  public void testCertsRotateNonRollingUpgrade(
      boolean currentNodeToNode,
      boolean currentClientToNode,
      boolean currentRootAndClientRootCASame,
      boolean rotateRootCA,
      boolean rotateClientRootCA,
      boolean rootAndClientRootCASame)
      throws IOException, NoSuchAlgorithmException {
    UUID rootCA = UUID.randomUUID();
    UUID clientRootCA = UUID.randomUUID();
    prepareUniverse(
        currentNodeToNode,
        currentClientToNode,
        currentRootAndClientRootCASame,
        rootCA,
        clientRootCA);
    CertsRotateParams taskParams =
        getTaskParams(
            rotateRootCA,
            rotateClientRootCA,
            rootAndClientRootCASame,
            UpgradeOption.NON_ROLLING_UPGRADE);

    TaskInfo taskInfo = submitTask(taskParams);
    if (taskInfo == null) {
      fail();
    }

    boolean isRootCARequired =
        CertificateHelper.isRootCARequired(
            currentNodeToNode, currentClientToNode, rootAndClientRootCASame);
    boolean isClientRootCARequired =
        CertificateHelper.isClientRootCARequired(
            currentNodeToNode, currentClientToNode, rootAndClientRootCASame);

    // Expected failure scenarios
    if ((!isRootCARequired && rotateRootCA)
        || (!isClientRootCARequired && rotateClientRootCA)
        || (isClientRootCARequired && !rotateClientRootCA && currentRootAndClientRootCASame)
        || (!rotateRootCA && !rotateClientRootCA)) {
      if (!(!rotateRootCA
          && !rotateClientRootCA
          && currentNodeToNode
          && currentClientToNode
          && !currentRootAndClientRootCASame
          && rootAndClientRootCASame)) {
        assertEquals(Failure, taskInfo.getTaskState());
        assertEquals(0, taskInfo.getSubTasks().size());
        verify(mockNodeManager, times(0)).nodeCommand(any(), any());
        return;
      }
    }

    assertEquals(100.0, taskInfo.getPercentCompleted(), 0);
    assertEquals(Success, taskInfo.getTaskState());

    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(TaskInfo::getPosition));

    int position = 0;
    // RootCA update task
    int expectedPosition = 14;
    if (rotateRootCA) {
      expectedPosition += 2;
      position = assertCommonTasks(subTasksByPosition, position, true, false);
    }
    // Cert update tasks
    position = assertCommonTasks(subTasksByPosition, position, false, false);
    // Restart tasks
    position = assertRestartSequence(subTasksByPosition, position, false);
    // RootCA update task
    if (rotateRootCA) {
      position = assertCommonTasks(subTasksByPosition, position, true, false);
    }
    // gflags update tasks
    position = assertCommonTasks(subTasksByPosition, position, false, false);
    position = assertCommonTasks(subTasksByPosition, position, false, false);
    // Update universe params task
    position = assertCommonTasks(subTasksByPosition, position, false, true);

    assertEquals(expectedPosition, position);
    verify(mockNodeManager, times(21)).nodeCommand(any(), any());

    assertUniverseDetails(
        taskParams,
        rootCA,
        clientRootCA,
        currentNodeToNode,
        currentClientToNode,
        rootAndClientRootCASame,
        rotateRootCA,
        rotateClientRootCA,
        isRootCARequired,
        isClientRootCARequired);
  }

  @Test
  @Parameters({
    "false, false, false, false, false, false",
    "false, false, false, false, false, true",
    "false, false, false, false, true, false",
    "false, false, false, false, true, true",
    "false, false, false, true, false, false",
    "false, false, false, true, false, true",
    "false, false, false, true, true, false",
    "false, false, false, true, true, true",
    "false, false, true, false, false, false",
    "false, false, true, false, false, true",
    "false, false, true, false, true, false",
    "false, false, true, false, true, true",
    "false, false, true, true, false, false",
    "false, false, true, true, false, true",
    "false, false, true, true, true, false",
    "false, false, true, true, true, true",
    "false, true, false, false, false, false",
    "false, true, false, false, false, true",
    "false, true, false, false, true, false",
    "false, true, false, false, true, true",
    "false, true, false, true, false, false",
    "false, true, false, true, false, true",
    "false, true, false, true, true, false",
    "false, true, false, true, true, true",
    "false, true, true, false, false, false",
    "false, true, true, false, false, true",
    "false, true, true, false, true, false",
    "false, true, true, false, true, true",
    "false, true, true, true, false, false",
    "false, true, true, true, false, true",
    "false, true, true, true, true, false",
    "false, true, true, true, true, true",
    "true, false, false, false, false, false",
    "true, false, false, false, false, true",
    "true, false, false, false, true, false",
    "true, false, false, false, true, true",
    "true, false, false, true, false, false",
    "true, false, false, true, false, true",
    "true, false, false, true, true, false",
    "true, false, false, true, true, true",
    "true, false, true, false, false, false",
    "true, false, true, false, false, true",
    "true, false, true, false, true, false",
    "true, false, true, false, true, true",
    "true, false, true, true, false, false",
    "true, false, true, true, false, true",
    "true, false, true, true, true, false",
    "true, false, true, true, true, true",
    "true, true, false, false, false, false",
    "true, true, false, false, false, true",
    "true, true, false, false, true, false",
    "true, true, false, false, true, true",
    "true, true, false, true, false, false",
    "true, true, false, true, false, true",
    "true, true, false, true, true, false",
    "true, true, false, true, true, true",
    "true, true, true, false, false, false",
    "true, true, true, false, false, true",
    "true, true, true, false, true, false",
    "true, true, true, false, true, true",
    "true, true, true, true, false, false",
    "true, true, true, true, false, true",
    "true, true, true, true, true, false",
    "true, true, true, true, true, true",
  })
  public void testCertsRotateRollingUpgrade(
      boolean currentNodeToNode,
      boolean currentClientToNode,
      boolean currentRootAndClientRootCASame,
      boolean rotateRootCA,
      boolean rotateClientRootCA,
      boolean rootAndClientRootCASame)
      throws IOException, NoSuchAlgorithmException {
    UUID rootCA = UUID.randomUUID();
    UUID clientRootCA = UUID.randomUUID();
    prepareUniverse(
        currentNodeToNode,
        currentClientToNode,
        currentRootAndClientRootCASame,
        rootCA,
        clientRootCA);
    CertsRotateParams taskParams =
        getTaskParams(
            rotateRootCA,
            rotateClientRootCA,
            rootAndClientRootCASame,
            UpgradeOption.ROLLING_UPGRADE);

    TaskInfo taskInfo = submitTask(taskParams);
    if (taskInfo == null) {
      fail();
    }

    boolean isRootCARequired =
        CertificateHelper.isRootCARequired(
            currentNodeToNode, currentClientToNode, rootAndClientRootCASame);
    boolean isClientRootCARequired =
        CertificateHelper.isClientRootCARequired(
            currentNodeToNode, currentClientToNode, rootAndClientRootCASame);

    // Expected failure scenarios
    if ((!isRootCARequired && rotateRootCA)
        || (!isClientRootCARequired && rotateClientRootCA)
        || (isClientRootCARequired && !rotateClientRootCA && currentRootAndClientRootCASame)
        || (!rotateRootCA && !rotateClientRootCA)) {
      if (!(!rotateRootCA
          && !rotateClientRootCA
          && currentNodeToNode
          && currentClientToNode
          && !currentRootAndClientRootCASame
          && rootAndClientRootCASame)) {
        assertEquals(TaskInfo.State.Failure, taskInfo.getTaskState());
        assertEquals(0, taskInfo.getSubTasks().size());
        verify(mockNodeManager, times(0)).nodeCommand(any(), any());
        return;
      }
    }

    assertEquals(100.0, taskInfo.getPercentCompleted(), 0);
    assertEquals(TaskInfo.State.Success, taskInfo.getTaskState());

    List<TaskInfo> subTasks = taskInfo.getSubTasks();
    Map<Integer, List<TaskInfo>> subTasksByPosition =
        subTasks.stream().collect(Collectors.groupingBy(TaskInfo::getPosition));

    int position = 0;
    int expectedPosition = 48;
    int expectedNumberOfInvocations = 21;
    if (rotateRootCA) {
      expectedPosition += 92;
      expectedNumberOfInvocations += 30;
      // RootCA update task
      position = assertCommonTasks(subTasksByPosition, position, true, false);
      // Cert update tasks
      position = assertCommonTasks(subTasksByPosition, position, false, false);
      // Restart tasks
      position = assertRestartSequence(subTasksByPosition, position, true);
      // Second round cert update tasks
      position = assertCommonTasks(subTasksByPosition, position, false, false);
      // Second round restart tasks
      position = assertRestartSequence(subTasksByPosition, position, true);
      // Third round cert update tasks
      position = assertCommonTasks(subTasksByPosition, position, false, false);
      // gflags update tasks
      position = assertCommonTasks(subTasksByPosition, position, false, false);
      position = assertCommonTasks(subTasksByPosition, position, false, false);
      // Update universe params task
      position = assertCommonTasks(subTasksByPosition, position, true, true);
      // Third round restart tasks
      position = assertRestartSequence(subTasksByPosition, position, true);
    } else {
      // Cert update tasks
      position = assertCommonTasks(subTasksByPosition, position, false, false);
      // Restart tasks
      position = assertRestartSequence(subTasksByPosition, position, true);
      // gflags update tasks
      position = assertCommonTasks(subTasksByPosition, position, false, false);
      position = assertCommonTasks(subTasksByPosition, position, false, false);
      // Update universe params task
      position = assertCommonTasks(subTasksByPosition, position, false, true);
    }

    assertEquals(expectedPosition, position);
    verify(mockNodeManager, times(expectedNumberOfInvocations)).nodeCommand(any(), any());

    assertUniverseDetails(
        taskParams,
        rootCA,
        clientRootCA,
        currentNodeToNode,
        currentClientToNode,
        rootAndClientRootCASame,
        rotateRootCA,
        rotateClientRootCA,
        isRootCARequired,
        isClientRootCARequired);
  }
}
