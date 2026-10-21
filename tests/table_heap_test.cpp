#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "table_heap.h"
#include "table_page.h"

class TableHeapTest : public ::testing::Test {
protected:
    const std::string db_file_ = "test_table_heap.db";

    void SetUp() override {
        if (std::filesystem::exists(db_file_)) {
            std::filesystem::remove(db_file_);
        }
    }

    void TearDown() override {
        if (std::filesystem::exists(db_file_)) {
            std::filesystem::remove(db_file_);
        }
    }
};

TEST_F(TableHeapTest, Initialization) {
    auto disk_manager = std::make_shared<DiskManager>(db_file_);
    auto bpm = std::make_shared<BufferPoolManager>(10, disk_manager);

    TableHeap heap(bpm);
    EXPECT_NE(heap.GetFirstPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(heap.GetFirstPageId(), 0);

    // Verify first page is initialized as TablePage
    Page *raw_page = bpm->FetchPage(heap.GetFirstPageId());
    ASSERT_NE(raw_page, nullptr);
    auto *table_page = TablePage::Cast(raw_page);
    EXPECT_EQ(table_page->GetTablePageId(), 0);
    EXPECT_EQ(table_page->GetPrevPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(table_page->GetNextPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(table_page->GetNumTuples(), 0);
    bpm->UnpinPage(heap.GetFirstPageId(), false);
}

TEST_F(TableHeapTest, InsertSingleTuple) {
    auto disk_manager = std::make_shared<DiskManager>(db_file_);
    auto bpm = std::make_shared<BufferPoolManager>(10, disk_manager);
    TableHeap heap(bpm);

    RID rid;
    std::string payload = "Hello TableHeap Single Record!";
    ASSERT_TRUE(heap.InsertTuple(Tuple(payload), &rid));

    EXPECT_EQ(rid.GetPageId(), heap.GetFirstPageId());
    EXPECT_EQ(rid.GetSlotNum(), 0);

    Tuple out_tuple;
    ASSERT_TRUE(heap.GetTuple(rid, &out_tuple));
    EXPECT_EQ(out_tuple.AsStringView(), payload);
    EXPECT_EQ(out_tuple.GetRid(), rid);

    // std::optional overload
    auto opt_tuple = heap.GetTuple(rid);
    ASSERT_TRUE(opt_tuple.has_value());
    EXPECT_EQ(opt_tuple->AsStringView(), payload);
}

TEST_F(TableHeapTest, MultiPageSpanningInsert) {
    auto disk_manager = std::make_shared<DiskManager>(db_file_);
    auto bpm = std::make_shared<BufferPoolManager>(20, disk_manager);
    TableHeap heap(bpm);

    // Insert enough 500-byte tuples to span across at least 3 pages
    // Each page holds ~4072 free space -> ~8 tuples of 500 bytes per page.
    const size_t total_tuples = 25;
    std::vector<RID> rids;
    std::vector<std::string> payloads;

    for (size_t i = 0; i < total_tuples; ++i) {
        std::string p = "Record_" + std::to_string(i) + "_" + std::string(450, 'A' + (i % 26));
        p.resize(500);

        RID r;
        ASSERT_TRUE(heap.InsertTuple(Tuple(p), &r)) << "Failed inserting tuple " << i;
        rids.push_back(r);
        payloads.push_back(p);
    }

    // Verify multiple pages were allocated and linked
    auto page_ids = heap.GetPageIds();
    EXPECT_GE(page_ids.size(), 3);
    EXPECT_EQ(page_ids[0], 0);

    // Verify doubly-linked page invariant
    for (size_t i = 0; i < page_ids.size(); ++i) {
        Page *raw = bpm->FetchPage(page_ids[i]);
        ASSERT_NE(raw, nullptr);
        auto *tp = TablePage::Cast(raw);

        if (i == 0) {
            EXPECT_EQ(tp->GetPrevPageId(), INVALID_PAGE_ID);
        } else {
            EXPECT_EQ(tp->GetPrevPageId(), page_ids[i - 1]);
        }

        if (i + 1 < page_ids.size()) {
            EXPECT_EQ(tp->GetNextPageId(), page_ids[i + 1]);
        } else {
            EXPECT_EQ(tp->GetNextPageId(), INVALID_PAGE_ID);
        }
        bpm->UnpinPage(page_ids[i], false);
    }

    // Verify all tuples across all pages read back identically
    for (size_t i = 0; i < total_tuples; ++i) {
        Tuple t;
        ASSERT_TRUE(heap.GetTuple(rids[i], &t));
        EXPECT_EQ(t.AsStringView(), payloads[i]);
        EXPECT_EQ(t.GetRid(), rids[i]);
    }
}

TEST_F(TableHeapTest, UpdateTupleInPlace) {
    auto disk_manager = std::make_shared<DiskManager>(db_file_);
    auto bpm = std::make_shared<BufferPoolManager>(10, disk_manager);
    TableHeap heap(bpm);

    RID rid;
    ASSERT_TRUE(heap.InsertTuple(Tuple("Original string 12345"), &rid));

    // Update with equal length
    RID new_rid;
    Tuple old_t;
    std::string updated_str = "Updated! string 54321";
    ASSERT_TRUE(heap.UpdateTuple(Tuple(updated_str), &old_t, rid, &new_rid));

    EXPECT_EQ(old_t.AsStringView(), "Original string 12345");
    EXPECT_EQ(new_rid, rid); // In-place, RID unchanged!

    Tuple read_t;
    ASSERT_TRUE(heap.GetTuple(rid, &read_t));
    EXPECT_EQ(read_t.AsStringView(), updated_str);

    // Update with smaller length
    std::string smaller_str = "Smaller";
    ASSERT_TRUE(heap.UpdateTuple(Tuple(smaller_str), &old_t, rid, &new_rid));
    EXPECT_EQ(old_t.AsStringView(), updated_str);
    EXPECT_EQ(new_rid, rid); // Same RID

    ASSERT_TRUE(heap.GetTuple(rid, &read_t));
    EXPECT_EQ(read_t.AsStringView(), smaller_str);
}

TEST_F(TableHeapTest, UpdateTupleGrowsWithinPage) {
    auto disk_manager = std::make_shared<DiskManager>(db_file_);
    auto bpm = std::make_shared<BufferPoolManager>(10, disk_manager);
    TableHeap heap(bpm);

    RID r0, r1;
    ASSERT_TRUE(heap.InsertTuple(Tuple("Short 0"), &r0));
    ASSERT_TRUE(heap.InsertTuple(Tuple("Short 1"), &r1));

    // Update r0 to grow to 500 bytes (fits on same page)
    std::string grown_data(500, 'G');
    RID new_r0;
    Tuple old_t0;
    ASSERT_TRUE(heap.UpdateTuple(Tuple(grown_data), &old_t0, r0, &new_r0));

    EXPECT_EQ(old_t0.AsStringView(), "Short 0");
    EXPECT_EQ(new_r0, r0); // Still on same page & same slot!

    Tuple read_t0, read_t1;
    ASSERT_TRUE(heap.GetTuple(r0, &read_t0));
    EXPECT_EQ(read_t0.AsStringView(), grown_data);

    ASSERT_TRUE(heap.GetTuple(r1, &read_t1));
    EXPECT_EQ(read_t1.AsStringView(), "Short 1");
}

TEST_F(TableHeapTest, UpdateTupleCrossPageMigration_DeleteOldInsertNew) {
    auto disk_manager = std::make_shared<DiskManager>(db_file_);
    auto bpm = std::make_shared<BufferPoolManager>(20, disk_manager);
    TableHeap heap(bpm);

    // 1. Fill page 0 almost completely with 4 tuples of 950 bytes
    // 4 * 950 = 3800 bytes data + 32 bytes slots + 24 header = 3856 bytes used.
    // Remaining free space on page 0 is ~240 bytes.
    RID r0, r1, r2, r3;
    ASSERT_TRUE(heap.InsertTuple(Tuple(std::string(950, '0')), &r0));
    ASSERT_TRUE(heap.InsertTuple(Tuple(std::string(950, '1')), &r1));
    ASSERT_TRUE(heap.InsertTuple(Tuple(std::string(950, '2')), &r2));
    ASSERT_TRUE(heap.InsertTuple(Tuple(std::string(950, '3')), &r3));

    EXPECT_EQ(r0.GetPageId(), 0);
    EXPECT_EQ(r1.GetPageId(), 0);
    EXPECT_EQ(r2.GetPageId(), 0);
    EXPECT_EQ(r3.GetPageId(), 0);

    // 2. Update r1 to grow from 950 to 1800 bytes.
    // Page 0 cannot fit 1800 bytes even with defragmentation (total free space on page 0 is only ~240 + 950 = 1190 < 1800)!
    // TableHeap MUST execute Strategy 3: Delete Old + Insert New on a new linked page!
    std::string huge_update(1800, 'U');
    RID new_rid;
    Tuple old_tuple;
    bool success = heap.UpdateTuple(Tuple(huge_update), &old_tuple, r1, &new_rid);
    ASSERT_TRUE(success);

    // Old tuple payload was captured
    EXPECT_EQ(old_tuple.AsStringView(), std::string(950, '1'));

    // The new RID MUST be on a different page (Page 1)!
    EXPECT_NE(new_rid.GetPageId(), r1.GetPageId());
    EXPECT_EQ(new_rid.GetPageId(), 1);

    // Old RID r1 is marked deleted on Page 0 and cannot be retrieved
    Tuple dead_tuple;
    EXPECT_FALSE(heap.GetTuple(r1, &dead_tuple));

    // New RID has the updated data
    Tuple migrated_tuple;
    ASSERT_TRUE(heap.GetTuple(new_rid, &migrated_tuple));
    EXPECT_EQ(migrated_tuple.AsStringView(), huge_update);
    EXPECT_EQ(migrated_tuple.GetRid(), new_rid);

    // Unrelated records on Page 0 (r0, r2, r3) are completely intact
    Tuple t0, t2, t3;
    ASSERT_TRUE(heap.GetTuple(r0, &t0));
    EXPECT_EQ(t0.AsStringView(), std::string(950, '0'));

    ASSERT_TRUE(heap.GetTuple(r2, &t2));
    EXPECT_EQ(t2.AsStringView(), std::string(950, '2'));

    ASSERT_TRUE(heap.GetTuple(r3, &t3));
    EXPECT_EQ(t3.AsStringView(), std::string(950, '3'));
}

TEST_F(TableHeapTest, MarkDeleteAndRollback) {
    auto disk_manager = std::make_shared<DiskManager>(db_file_);
    auto bpm = std::make_shared<BufferPoolManager>(10, disk_manager);
    TableHeap heap(bpm);

    RID r0, r1;
    ASSERT_TRUE(heap.InsertTuple(Tuple("Active row"), &r0));
    ASSERT_TRUE(heap.InsertTuple(Tuple("Temporary row"), &r1));

    // Mark delete r1
    EXPECT_TRUE(heap.MarkDelete(r1));

    Tuple t;
    EXPECT_TRUE(heap.GetTuple(r0, &t));
    EXPECT_FALSE(heap.GetTuple(r1, &t)); // Deleted

    // Rollback delete r1
    EXPECT_TRUE(heap.RollbackDelete(r1));
    EXPECT_TRUE(heap.GetTuple(r1, &t)); // Readable again!
    EXPECT_EQ(t.AsStringView(), "Temporary row");

    // Apply delete
    EXPECT_TRUE(heap.ApplyDelete(r1));
    EXPECT_FALSE(heap.GetTuple(r1, &t));
}

TEST_F(TableHeapTest, TableIteratorMultiPage) {
    auto disk_manager = std::make_shared<DiskManager>(db_file_);
    auto bpm = std::make_shared<BufferPoolManager>(20, disk_manager);
    TableHeap heap(bpm);

    // Insert 20 records (will span multiple pages)
    std::vector<std::string> payloads;
    std::vector<RID> rids;
    for (int i = 0; i < 20; ++i) {
        std::string s = "Row_" + std::to_string(i) + "_" + std::string(300, 'Z');
        s.resize(320);
        RID r;
        ASSERT_TRUE(heap.InsertTuple(Tuple(s), &r));
        payloads.push_back(s);
        rids.push_back(r);
    }

    // Delete a couple of rows across different pages
    ASSERT_TRUE(heap.MarkDelete(rids[2]));
    ASSERT_TRUE(heap.MarkDelete(rids[11]));

    // Iterate through entire table using range-for
    std::vector<std::string> collected;
    std::vector<RID> collected_rids;
    for (const auto &tuple : heap) {
        collected.emplace_back(tuple.AsStringView());
        collected_rids.push_back(tuple.GetRid());
    }

    // Should have exactly 18 live tuples
    ASSERT_EQ(collected.size(), 18);

    // Verify row 2 and row 11 were skipped
    for (const auto &r : collected_rids) {
        EXPECT_NE(r, rids[2]);
        EXPECT_NE(r, rids[11]);
    }

    // Verify iteration using Next() cursor idiom
    auto it = heap.begin();
    Tuple cur_tuple;
    RID cur_rid;
    size_t scan_count = 0;
    while (it.Next(&cur_tuple, &cur_rid)) {
        EXPECT_FALSE(cur_tuple.IsEmpty());
        EXPECT_TRUE(cur_rid.IsValid());
        scan_count++;
    }
    EXPECT_EQ(scan_count, 18);
}

TEST_F(TableHeapTest, PersistenceAcrossRestart) {
    page_id_t root_page_id = INVALID_PAGE_ID;
    std::vector<std::string> expected = {
        "Persistent record A",
        "Persistent record B - larger payload with variable bytes",
        "Persistent record C"
    };
    std::vector<RID> saved_rids;

    // Phase 1: Write data and flush to disk
    {
        auto disk_manager = std::make_shared<DiskManager>(db_file_);
        auto bpm = std::make_shared<BufferPoolManager>(10, disk_manager);
        TableHeap heap(bpm);
        root_page_id = heap.GetFirstPageId();

        for (const auto &str : expected) {
            RID r;
            ASSERT_TRUE(heap.InsertTuple(Tuple(str), &r));
            saved_rids.push_back(r);
        }

        bpm->FlushAllPages();
    }

    // Phase 2: Reopen with fresh BPM and attach TableHeap to root_page_id
    {
        auto disk_manager = std::make_shared<DiskManager>(db_file_);
        auto bpm = std::make_shared<BufferPoolManager>(10, disk_manager);
        TableHeap heap(bpm, root_page_id);

        EXPECT_EQ(heap.GetFirstPageId(), root_page_id);

        for (size_t i = 0; i < expected.size(); ++i) {
            Tuple t;
            ASSERT_TRUE(heap.GetTuple(saved_rids[i], &t));
            EXPECT_EQ(t.AsStringView(), expected[i]);
            EXPECT_EQ(t.GetRid(), saved_rids[i]);
        }
    }
}
