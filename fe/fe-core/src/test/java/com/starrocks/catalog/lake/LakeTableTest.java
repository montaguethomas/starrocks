// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

package com.starrocks.catalog.lake;

import com.google.common.collect.Lists;
import com.staros.proto.ObjectStorageInfo;
import com.staros.proto.ShardStorageInfo;
import com.starrocks.catalog.AggregateType;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.DistributionInfo;
import com.starrocks.catalog.HashDistributionInfo;
import com.starrocks.catalog.KeysType;
import com.starrocks.catalog.MaterializedIndex;
import com.starrocks.catalog.Partition;
import com.starrocks.catalog.PartitionInfo;
import com.starrocks.catalog.SinglePartitionInfo;
import com.starrocks.catalog.Table;
import com.starrocks.catalog.Tablet;
import com.starrocks.catalog.TabletMeta;
import com.starrocks.catalog.Type;
import com.starrocks.common.DdlException;
import com.starrocks.common.FeConstants;
import com.starrocks.common.io.FastByteArrayOutputStream;
import com.starrocks.common.jmockit.Deencapsulation;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.thrift.TStorageMedium;
import com.starrocks.thrift.TStorageType;
import mockit.Mock;
import mockit.MockUp;
import org.junit.Assert;
import org.junit.Test;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.util.List;

public class LakeTableTest {

    @Test
    public void testLakeTable() throws IOException, DdlException {
        new MockUp<GlobalStateMgr>() {
            @Mock
            int getCurrentStateJournalVersion() {
                return FeConstants.meta_version;
            }
        };

        long dbId = 1L;
        long tableId = 2L;
        long partitionId = 3L;
        long indexId = 4L;
        long tablet1Id = 10L;
        long tablet2Id = 11L;
        String serviceStorageUri = "s3://bucket/service/";
        String endpoint = "region.host.com";

        // Schema
        List<Column> columns = Lists.newArrayList();
        Column k1 = new Column("k1", Type.INT, true, null, "", "");
        columns.add(k1);
        columns.add(new Column("k2", Type.BIGINT, true, null, "", ""));
        columns.add(new Column("v", Type.BIGINT, false, AggregateType.SUM, "0", ""));

        // Tablet
        Tablet tablet1 = new LakeTablet(tablet1Id);
        Tablet tablet2 = new LakeTablet(tablet2Id);

        // Index
        MaterializedIndex index = new MaterializedIndex(indexId, MaterializedIndex.IndexState.NORMAL);
        TabletMeta tabletMeta = new TabletMeta(dbId, tableId, partitionId, indexId, 0, TStorageMedium.HDD);
        index.addTablet(tablet1, tabletMeta);
        index.addTablet(tablet2, tabletMeta);

        // Partition
        DistributionInfo distributionInfo = new HashDistributionInfo(10, Lists.newArrayList(k1));
        PartitionInfo partitionInfo = new SinglePartitionInfo();
        partitionInfo.setReplicationNum(partitionId, (short) 3);
        Partition partition = new Partition(partitionId, "p1", index, distributionInfo);

        // Lake table
        LakeTable table = new LakeTable(tableId, "t1", columns, KeysType.AGG_KEYS, partitionInfo, distributionInfo);
        Deencapsulation.setField(table, "baseIndexId", indexId);
        table.addPartition(partition);
        table.setIndexMeta(indexId, "t1", columns, 0, 0, (short) 3, TStorageType.COLUMN, KeysType.AGG_KEYS);
        ObjectStorageInfo objectStorageInfo =
                ObjectStorageInfo.newBuilder().setObjectUri(serviceStorageUri).setEndpoint(endpoint).build();
        ShardStorageInfo shardStorageInfo =
                ShardStorageInfo.newBuilder().setObjectStorageInfo(objectStorageInfo).build();
        table.setShardStorageInfo(shardStorageInfo);

        // Test serialize and deserialize
        FastByteArrayOutputStream byteArrayOutputStream = new FastByteArrayOutputStream();
        try (DataOutputStream out = new DataOutputStream(byteArrayOutputStream)) {
            table.write(out);
            out.flush();
        }

        Table newTable = null;
        try (DataInputStream in = new DataInputStream(byteArrayOutputStream.getInputStream())) {
            newTable = Table.read(in);
        }
        byteArrayOutputStream.close();

        // Check lake table and lake tablet
        Assert.assertTrue(newTable.isLakeTable());
        LakeTable newLakeTable = (LakeTable) newTable;
        Assert.assertEquals(String.format("%s%d/", serviceStorageUri, tableId), newLakeTable.getStorageGroup());
        ObjectStorageInfo newObjectStorageInfo = newLakeTable.getShardStorageInfo().getObjectStorageInfo();
        Assert.assertEquals(endpoint, newObjectStorageInfo.getEndpoint());

        Partition p1 = newLakeTable.getPartition(partitionId);
        MaterializedIndex newIndex = p1.getBaseIndex();
        long expectedTabletId = 10L;
        for (Tablet tablet : newIndex.getTablets()) {
            Assert.assertTrue(tablet instanceof LakeTablet);
            LakeTablet lakeTablet = (LakeTablet) tablet;
            Assert.assertEquals(expectedTabletId, lakeTablet.getId());
            Assert.assertEquals(expectedTabletId, lakeTablet.getShardId());
            ++expectedTabletId;
        }
    }
}
