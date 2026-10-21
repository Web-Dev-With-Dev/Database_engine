#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "buffer_pool_manager.h"
#include "disk_manager.h"
#include "table_page.h"

class TablePageTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TablePageTest, Initialization) {
    TablePage page;
    page.Init(42, 10, 20);

    EXPECT_EQ(page.GetTablePageId(), 42);
    EXPECT_EQ(page.GetPageId(), 42);
    EXPECT_EQ(page.GetPrevPageId(), 10);
    EXPECT_EQ(page.GetNextPageId(), 20);
    EXPECT_EQ(page.GetNumTuples(), 0);
    EXPECT_EQ(page.GetFreeSpacePointer(), PAGE_SIZE);
    EXPECT_EQ(page.GetFreeSpaceRemaining(), PAGE_SIZE - TablePage::SIZE_TABLE_PAGE_HEADER);
    EXPECT_TRUE(page.IsDirty());
}

TEST_F(TablePageTest, HeaderSetters) {
    TablePage page;
    page.Init(1);

    page.SetPrevPageId(99);
    EXPECT_EQ(page.GetPrevPageId(), 99);

    page.SetNextPageId(101);
    EXPECT_EQ(page.GetNextPageId(), 101);

    page.SetLSN(12345);
    EXPECT_EQ(page.GetLSN(), 12345);

    page.SetTablePageId(77);
    EXPECT_EQ(page.GetTablePageId(), 77);
    EXPECT_EQ(page.GetPageId(), 77);
}

TEST_F(TablePageTest, InsertSingleTuple) {
    TablePage page;
    page.Init(1);

    std::string payload = "Hello, Slotted Page!";
    Tuple tuple(payload);
    RID rid;

    bool success = page.InsertTuple(tuple, &rid);
    EXPECT_TRUE(success);
    EXPECT_EQ(rid.GetPageId(), 1);
    EXPECT_EQ(rid.GetSlotNum(), 0);

    EXPECT_EQ(page.GetNumTuples(), 1);
    EXPECT_EQ(page.GetFreeSpacePointer(), PAGE_SIZE - payload.size());
    EXPECT_EQ(page.GetTupleOffset(0), PAGE_SIZE - payload.size());
    EXPECT_EQ(page.GetTupleSize(0), payload.size());
    EXPECT_FALSE(page.IsTupleDeleted(0));

    // Verify Read
    Tuple read_tuple;
    EXPECT_TRUE(page.GetTuple(rid, &read_tuple));
    EXPECT_EQ(read_tuple.AsStringView(), payload);
    EXPECT_EQ(read_tuple.GetRid(), rid);
}

TEST_F(TablePageTest, InsertMultipleVariableLengthTuples) {
    TablePage page;
    page.Init(5);

    std::vector<std::string> payloads = {
        "First short tuple",
        "A slightly longer second tuple with more bytes to test variable length capacity",
        "Third",
        std::string(300, 'Z'),
        std::string(1000, 'X')
    };

    std::vector<RID> rids;
    for (const auto &p : payloads) {
        RID rid;
        EXPECT_TRUE(page.InsertTuple(Tuple(p), &rid));
        rids.push_back(rid);
    }

    EXPECT_EQ(page.GetNumTuples(), payloads.size());

    // Verify all tuples read back identically
    for (size_t i = 0; i < payloads.size(); ++i) {
        Tuple read_tuple;
        EXPECT_TRUE(page.GetTuple(rids[i], &read_tuple));
        EXPECT_EQ(read_tuple.AsStringView(), payloads[i]);
    }
}

TEST_F(TablePageTest, UpdateTupleInPlace) {
    TablePage page;
    page.Init(1);

    std::string orig = "Original string 1234567890";
    RID rid;
    EXPECT_TRUE(page.InsertTuple(Tuple(orig), &rid));

    uint32_t orig_offset = page.GetTupleOffset(0);

    // Update with equal length
    std::string updated_same = "Updated! string 0987654321";
    Tuple old_tuple;
    EXPECT_TRUE(page.UpdateTuple(Tuple(updated_same), &old_tuple, rid));
    EXPECT_EQ(old_tuple.AsStringView(), orig);
    EXPECT_EQ(page.GetTupleOffset(0), orig_offset); // In-place, offset unchanged

    Tuple read_tuple;
    EXPECT_TRUE(page.GetTuple(rid, &read_tuple));
    EXPECT_EQ(read_tuple.AsStringView(), updated_same);

    // Update with smaller length
    std::string smaller = "Smaller";
    EXPECT_TRUE(page.UpdateTuple(Tuple(smaller), &old_tuple, rid));
    EXPECT_EQ(old_tuple.AsStringView(), updated_same);
    EXPECT_EQ(page.GetTupleOffset(0), orig_offset); // In-place
    EXPECT_EQ(page.GetTupleSize(0), smaller.size());

    EXPECT_TRUE(page.GetTuple(rid, &read_tuple));
    EXPECT_EQ(read_tuple.AsStringView(), smaller);
}

TEST_F(TablePageTest, UpdateTupleGrow) {
    TablePage page;
    page.Init(1);

    std::string short_str = "Tiny";
    RID rid;
    EXPECT_TRUE(page.InsertTuple(Tuple(short_str), &rid));

    uint32_t orig_free_ptr = page.GetFreeSpacePointer();

    std::string long_str = "This is a substantially longer replacement tuple that cannot fit in the old slot";
    Tuple old_tuple;
    EXPECT_TRUE(page.UpdateTuple(Tuple(long_str), &old_tuple, rid));
    EXPECT_EQ(old_tuple.AsStringView(), short_str);

    // Free space pointer should have decremented to allocate the larger tuple
    EXPECT_EQ(page.GetFreeSpacePointer(), orig_free_ptr - long_str.size());
    EXPECT_EQ(page.GetTupleOffset(0), page.GetFreeSpacePointer());
    EXPECT_EQ(page.GetTupleSize(0), long_str.size());

    Tuple read_tuple;
    EXPECT_TRUE(page.GetTuple(rid, &read_tuple));
    EXPECT_EQ(read_tuple.AsStringView(), long_str);
}

TEST_F(TablePageTest, MarkDeleteAndRollback) {
    TablePage page;
    page.Init(1);

    std::string payload = "Active record";
    RID rid;
    EXPECT_TRUE(page.InsertTuple(Tuple(payload), &rid));

    EXPECT_FALSE(page.IsTupleDeleted(rid.GetSlotNum()));

    // Mark delete
    EXPECT_TRUE(page.MarkDelete(rid));
    EXPECT_TRUE(page.IsTupleDeleted(rid.GetSlotNum()));

    // Cannot read marked-deleted tuple
    Tuple tuple;
    EXPECT_FALSE(page.GetTuple(rid, &tuple));

    // Rollback delete
    EXPECT_TRUE(page.RollbackDelete(rid));
    EXPECT_FALSE(page.IsTupleDeleted(rid.GetSlotNum()));

    EXPECT_TRUE(page.GetTuple(rid, &tuple));
    EXPECT_EQ(tuple.AsStringView(), payload);
}

TEST_F(TablePageTest, ApplyDeleteAndSlotReuse) {
    TablePage page;
    page.Init(1);

    RID rid0, rid1;
    EXPECT_TRUE(page.InsertTuple(Tuple("Tuple 0"), &rid0));
    EXPECT_TRUE(page.InsertTuple(Tuple("Tuple 1"), &rid1));
    EXPECT_EQ(page.GetNumTuples(), 2);

    // Apply delete on slot 0
    EXPECT_TRUE(page.ApplyDelete(rid0));
    EXPECT_TRUE(page.IsTupleDeleted(0));
    EXPECT_EQ(page.GetTupleOffset(0), 0);
    EXPECT_EQ(page.GetTupleSize(0), 0);

    // Inserting next tuple should reuse slot 0 without incrementing num_tuples
    RID rid_new;
    std::string new_payload = "Reused Slot 0 Record!";
    EXPECT_TRUE(page.InsertTuple(Tuple(new_payload), &rid_new));
    EXPECT_EQ(rid_new.GetSlotNum(), 0);
    EXPECT_EQ(page.GetNumTuples(), 2); // Did not increase!

    Tuple read_tuple;
    EXPECT_TRUE(page.GetTuple(rid_new, &read_tuple));
    EXPECT_EQ(read_tuple.AsStringView(), new_payload);
}

TEST_F(TablePageTest, PageFullRejection) {
    TablePage page;
    page.Init(1);

    // Insert a huge tuple almost filling the page
    // Free space remaining initially: 4096 - 24 = 4072
    // We need 8 bytes for slot, so max tuple length is 4072 - 8 = 4064
    uint32_t max_fit = 4064;
    std::string huge(max_fit, 'H');
    RID rid1;
    EXPECT_TRUE(page.InsertTuple(Tuple(huge), &rid1));

    EXPECT_EQ(page.GetFreeSpaceRemaining(), 0);

    // Any further insertion must fail
    RID rid2;
    EXPECT_FALSE(page.InsertTuple(Tuple("Even 1 byte fails"), &rid2));
}

TEST_F(TablePageTest, Defragmentation) {
    TablePage page;
    page.Init(1);

    // Insert 4 tuples: T0, T1, T2, T3
    RID r0, r1, r2, r3;
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(100, '0')), &r0));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(200, '1')), &r1));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(300, '2')), &r2));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(400, '3')), &r3));

    uint32_t free_before = page.GetFreeSpaceRemaining();

    // Delete T1 and T3
    EXPECT_TRUE(page.ApplyDelete(r1));
    EXPECT_TRUE(page.ApplyDelete(r3));

    // Defragment the page
    page.Defragment();

    // Free space should have expanded by the freed tuples (200 + 400 = 600) + trailing slot reclaimed (8 bytes for T3)
    uint32_t free_after = page.GetFreeSpaceRemaining();
    EXPECT_EQ(free_after, free_before + 600 + TablePage::SIZEOF_TUPLE_SLOT);

    // Live tuples T0 and T2 must still be fully readable and intact
    Tuple read0, read2;
    EXPECT_TRUE(page.GetTuple(r0, &read0));
    EXPECT_EQ(read0.AsStringView(), std::string(100, '0'));

    EXPECT_TRUE(page.GetTuple(r2, &read2));
    EXPECT_EQ(read2.AsStringView(), std::string(300, '2'));

    // T1 and T3 must be deleted
    Tuple dead_tuple;
    EXPECT_FALSE(page.GetTuple(r1, &dead_tuple));
    EXPECT_FALSE(page.GetTuple(r3, &dead_tuple));
}

TEST_F(TablePageTest, TupleIteration) {
    TablePage page;
    page.Init(1);

    RID r0, r1, r2, r3;
    EXPECT_TRUE(page.InsertTuple(Tuple("Item 0"), &r0));
    EXPECT_TRUE(page.InsertTuple(Tuple("Item 1"), &r1));
    EXPECT_TRUE(page.InsertTuple(Tuple("Item 2"), &r2));
    EXPECT_TRUE(page.InsertTuple(Tuple("Item 3"), &r3));

    // Delete Item 1
    page.MarkDelete(r1);

    // Traverse live tuples
    RID cur;
    EXPECT_TRUE(page.GetFirstTupleRid(&cur));
    EXPECT_EQ(cur, r0);

    EXPECT_TRUE(page.GetNextTupleRid(cur, &cur));
    EXPECT_EQ(cur, r2); // Skipped r1

    EXPECT_TRUE(page.GetNextTupleRid(cur, &cur));
    EXPECT_EQ(cur, r3);

    EXPECT_FALSE(page.GetNextTupleRid(cur, &cur)); // End of page
}

TEST_F(TablePageTest, CastFromRawPage) {
    Page raw_page;
    raw_page.SetPageId(10);

    TablePage *table_page = TablePage::Cast(&raw_page);
    table_page->Init(10);

    RID rid;
    EXPECT_TRUE(table_page->InsertTuple(Tuple("Direct cast works!"), &rid));

    Tuple out;
    EXPECT_TRUE(table_page->GetTuple(rid, &out));
    EXPECT_EQ(out.AsStringView(), "Direct cast works!");
    EXPECT_TRUE(raw_page.IsDirty());
}

TEST_F(TablePageTest, BufferPoolManagerPersistence) {
    const std::string db_file = "test_table_page_bpm.db";
    if (std::filesystem::exists(db_file)) {
        std::filesystem::remove(db_file);
    }

    {
        auto disk_manager = std::make_shared<DiskManager>(db_file);
        BufferPoolManager bpm(5, disk_manager);

        page_id_t page_id = INVALID_PAGE_ID;
        Page *raw_page = bpm.NewPage(&page_id);
        ASSERT_NE(raw_page, nullptr);
        EXPECT_EQ(page_id, 0);

        auto *table_page = TablePage::Cast(raw_page);
        table_page->Init(page_id, INVALID_PAGE_ID, INVALID_PAGE_ID);

        RID rid1, rid2;
        EXPECT_TRUE(table_page->InsertTuple(Tuple("Row 1: Persisted in BufferPool"), &rid1));
        EXPECT_TRUE(table_page->InsertTuple(Tuple("Row 2: Variable length slotted tuple"), &rid2));

        bpm.UnpinPage(page_id, true);
        bpm.FlushAllPages();
    }

    // Reopen database file with fresh BPM and verify persistence
    {
        auto disk_manager = std::make_shared<DiskManager>(db_file);
        BufferPoolManager bpm(5, disk_manager);

        Page *raw_page = bpm.FetchPage(0);
        ASSERT_NE(raw_page, nullptr);

        auto *table_page = TablePage::Cast(raw_page);
        EXPECT_EQ(table_page->GetTablePageId(), 0);
        EXPECT_EQ(table_page->GetNumTuples(), 2);

        Tuple t1, t2;
        EXPECT_TRUE(table_page->GetTuple(RID(0, 0), &t1));
        EXPECT_EQ(t1.AsStringView(), "Row 1: Persisted in BufferPool");

        EXPECT_TRUE(table_page->GetTuple(RID(0, 1), &t2));
        EXPECT_EQ(t2.AsStringView(), "Row 2: Variable length slotted tuple");

        bpm.UnpinPage(0, false);
    }

    if (std::filesystem::exists(db_file)) {
        std::filesystem::remove(db_file);
    }
}

TEST_F(TablePageTest, FreeSpaceAndFragmentationCalculations) {
    TablePage page;
    page.Init(1);

    // Initial state: empty page
    uint32_t expected_initial_free = PAGE_SIZE - TablePage::SIZE_TABLE_PAGE_HEADER;
    EXPECT_EQ(page.GetFreeSpaceRemaining(), expected_initial_free);
    EXPECT_EQ(page.GetTotalFreeSpace(), expected_initial_free);
    EXPECT_EQ(page.GetFragmentedSpace(), 0);

    // Insert 3 tuples of 500 bytes each
    RID r0, r1, r2;
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(500, 'A')), &r0));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(500, 'B')), &r1));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(500, 'C')), &r2));

    // Slots: 3 * 8 = 24 bytes. Tuples: 1500 bytes.
    // Total used: 24 (hdr) + 24 (slots) + 1500 = 1548 bytes.
    // Free space remaining: 4096 - 1548 = 2548.
    EXPECT_EQ(page.GetFreeSpaceRemaining(), 2548);
    EXPECT_EQ(page.GetTotalFreeSpace(), 2548);
    EXPECT_EQ(page.GetFragmentedSpace(), 0);

    // Mark delete tuple 1 (500 bytes)
    EXPECT_TRUE(page.MarkDelete(r1));
    EXPECT_TRUE(page.IsTupleDeleted(r1.GetSlotNum()));

    // Contiguous free space remains 2548, but total free space is 2548 + 500 = 3048
    EXPECT_EQ(page.GetFreeSpaceRemaining(), 2548);
    EXPECT_EQ(page.GetTotalFreeSpace(), 3048);
    EXPECT_EQ(page.GetFragmentedSpace(), 500);

    // Mark delete tuple 2 (both slots 1 and 2 now become trailing deleted slots: 500 bytes + 2 * 8 bytes slot reclamation)
    EXPECT_TRUE(page.MarkDelete(r2));
    EXPECT_EQ(page.GetFreeSpaceRemaining(), 2548);
    EXPECT_EQ(page.GetTotalFreeSpace(), 3048 + 500 + 2 * TablePage::SIZEOF_TUPLE_SLOT);
    EXPECT_EQ(page.GetFragmentedSpace(), 1016);

    // Manual defragmentation consolidates all free space
    page.Defragment();
    EXPECT_EQ(page.GetFragmentedSpace(), 0);
    EXPECT_EQ(page.GetFreeSpaceRemaining(), page.GetTotalFreeSpace());
    EXPECT_EQ(page.GetNumTuples(), 1); // Trailing slots r1 and r2 truncated

    // Live tuple r0 is still readable
    Tuple out;
    EXPECT_TRUE(page.GetTuple(r0, &out));
    EXPECT_EQ(out.AsStringView(), std::string(500, 'A'));
}

TEST_F(TablePageTest, InsertTupleCompactionOnMarkDelete) {
    TablePage page;
    page.Init(1);

    // Fill page almost completely with 4 tuples of 950 bytes
    // Total tuple data: 4 * 950 = 3800 bytes
    // Header (24) + Slots (4 * 8 = 32) + 3800 = 3856 bytes used
    // Contiguous free space: 4096 - 3856 = 240 bytes
    RID r0, r1, r2, r3;
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(950, '0')), &r0));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(950, '1')), &r1));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(950, '2')), &r2));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(950, '3')), &r3));

    EXPECT_EQ(page.GetFreeSpaceRemaining(), 240);

    // Mark delete r1 (950 bytes dead in the middle)
    EXPECT_TRUE(page.MarkDelete(r1));

    // Try to insert an 800-byte tuple.
    // Contiguous space is only 240 bytes (< 800), but total free space is 240 + 950 = 1190 bytes (>= 800).
    // InsertTuple must automatically trigger defragmentation/compaction!
    RID r_new;
    std::string new_data(800, 'N');
    EXPECT_TRUE(page.InsertTuple(Tuple(new_data), &r_new));

    // r_new should reuse slot 1 (since r1 was defragmented/cleared)
    EXPECT_EQ(r_new.GetSlotNum(), 1);
    EXPECT_EQ(page.GetNumTuples(), 4);

    // All active tuples (0, new, 2, 3) must be intact and verified
    Tuple t0, t_new, t2, t3;
    EXPECT_TRUE(page.GetTuple(r0, &t0));
    EXPECT_EQ(t0.AsStringView(), std::string(950, '0'));

    EXPECT_TRUE(page.GetTuple(r_new, &t_new));
    EXPECT_EQ(t_new.AsStringView(), new_data);

    EXPECT_TRUE(page.GetTuple(r2, &t2));
    EXPECT_EQ(t2.AsStringView(), std::string(950, '2'));

    EXPECT_TRUE(page.GetTuple(r3, &t3));
    EXPECT_EQ(t3.AsStringView(), std::string(950, '3'));
}

TEST_F(TablePageTest, InsertTupleCompactionOnApplyDelete) {
    TablePage page;
    page.Init(1);

    // Fill page with 4 tuples of 900 bytes
    RID r0, r1, r2, r3;
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(900, 'A')), &r0));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(900, 'B')), &r1));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(900, 'C')), &r2));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(900, 'D')), &r3));

    // Apply delete on r1 and trailing r3
    EXPECT_TRUE(page.ApplyDelete(r1));
    EXPECT_TRUE(page.ApplyDelete(r3));

    // Contiguous free space is still small before compaction
    uint32_t contig_before = page.GetFreeSpaceRemaining();
    EXPECT_LT(contig_before, 500);

    // Insert 1200-byte tuple which requires compaction to succeed
    RID r_large;
    std::string large_payload(1200, 'L');
    EXPECT_TRUE(page.InsertTuple(Tuple(large_payload), &r_large));

    // Verify data integrity
    Tuple t0, t2, t_large;
    EXPECT_TRUE(page.GetTuple(r0, &t0));
    EXPECT_EQ(t0.AsStringView(), std::string(900, 'A'));

    EXPECT_TRUE(page.GetTuple(r2, &t2));
    EXPECT_EQ(t2.AsStringView(), std::string(900, 'C'));

    EXPECT_TRUE(page.GetTuple(r_large, &t_large));
    EXPECT_EQ(t_large.AsStringView(), large_payload);
}

TEST_F(TablePageTest, UpdateTupleCompaction) {
    TablePage page;
    page.Init(1);

    // Insert 3 tuples
    RID r0, r1, r2;
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(1000, '0')), &r0));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(1000, '1')), &r1));
    EXPECT_TRUE(page.InsertTuple(Tuple(std::string(1000, '2')), &r2));

    // Mark delete r1 (1000 bytes dead)
    EXPECT_TRUE(page.MarkDelete(r1));

    // Update r0 to grow from 1000 to 1800 bytes
    // Contiguous space is ~1048 bytes, not enough for 1800.
    // But compaction reclaims r1 (1000) and r0's old space (1000), giving plenty of room!
    std::string grown_data(1800, 'G');
    Tuple old_tuple;
    EXPECT_TRUE(page.UpdateTuple(Tuple(grown_data), &old_tuple, r0));
    EXPECT_EQ(old_tuple.AsStringView(), std::string(1000, '0'));

    // Verify r0 now has grown data and r2 is intact
    Tuple t0, t2;
    EXPECT_TRUE(page.GetTuple(r0, &t0));
    EXPECT_EQ(t0.AsStringView(), grown_data);

    EXPECT_TRUE(page.GetTuple(r2, &t2));
    EXPECT_EQ(t2.AsStringView(), std::string(1000, '2'));
}

TEST_F(TablePageTest, MarkDeleteValidation) {
    TablePage page;
    page.Init(10);

    RID r0;
    EXPECT_TRUE(page.InsertTuple(Tuple("Valid Tuple"), &r0));

    // Wrong page ID
    EXPECT_FALSE(page.MarkDelete(RID(999, 0)));

    // Out of bounds slot
    EXPECT_FALSE(page.MarkDelete(RID(10, 5)));

    // Valid delete
    EXPECT_TRUE(page.MarkDelete(r0));

    // Already deleted
    EXPECT_FALSE(page.MarkDelete(r0));
}

TEST_F(TablePageTest, MultipleCompactionCycles) {
    TablePage page;
    page.Init(1);

    // Repeated cycles of insert, delete, and compacting insert
    for (int cycle = 0; cycle < 5; ++cycle) {
        RID rA, rB;
        EXPECT_TRUE(page.InsertTuple(Tuple(std::string(1200, 'A' + cycle)), &rA));
        EXPECT_TRUE(page.InsertTuple(Tuple(std::string(1200, 'B' + cycle)), &rB));

        EXPECT_TRUE(page.MarkDelete(rA));

        // Insert new tuple that forces compaction to reclaim rA
        RID rC;
        EXPECT_TRUE(page.InsertTuple(Tuple(std::string(1200, 'C' + cycle)), &rC));

        Tuple tB, tC;
        EXPECT_TRUE(page.GetTuple(rB, &tB));
        EXPECT_EQ(tB.AsStringView(), std::string(1200, 'B' + cycle));

        EXPECT_TRUE(page.GetTuple(rC, &tC));
        EXPECT_EQ(tC.AsStringView(), std::string(1200, 'C' + cycle));

        // Clean up for next cycle
        EXPECT_TRUE(page.MarkDelete(rB));
        EXPECT_TRUE(page.MarkDelete(rC));
    }
}

// ============================================================================
// Page-level Tuple Iterator & GetTuple Unit Tests
// ============================================================================

TEST_F(TablePageTest, IteratorEmptyPage) {
    TablePage page;
    page.Init(10);

    EXPECT_EQ(page.begin(), page.end());
    EXPECT_EQ(page.cbegin(), page.cend());
    EXPECT_EQ(page.Begin(), page.End());

    auto it = page.begin();
    EXPECT_TRUE(it.IsEnd());
    EXPECT_FALSE(it.HasNext());
    EXPECT_FALSE(it.IsValid());
    EXPECT_FALSE(static_cast<bool>(it));

    Tuple tuple;
    EXPECT_FALSE(it.GetTuple(&tuple));
    RID rid;
    EXPECT_FALSE(it.Next(&tuple, &rid));

    int count = 0;
    for (const auto &t : page) {
        (void)t;
        count++;
    }
    EXPECT_EQ(count, 0);
}

TEST_F(TablePageTest, IteratorSingleLiveTuple) {
    TablePage page;
    page.Init(1);

    RID rid;
    ASSERT_TRUE(page.InsertTuple(Tuple("Only tuple"), &rid));

    auto it = page.begin();
    ASSERT_NE(it, page.end());
    EXPECT_FALSE(it.IsEnd());
    EXPECT_TRUE(it.HasNext());
    EXPECT_TRUE(static_cast<bool>(it));

    EXPECT_EQ(it.GetSlotNum(), 0);
    EXPECT_EQ(it.GetRid(), rid);
    EXPECT_EQ(it.GetPageId(), 1);
    EXPECT_EQ(it->AsStringView(), "Only tuple");
    EXPECT_EQ((*it).AsStringView(), "Only tuple");
    EXPECT_EQ(it.GetTuple().AsStringView(), "Only tuple");

    Tuple out_t;
    EXPECT_TRUE(it.GetTuple(&out_t));
    EXPECT_EQ(out_t.AsStringView(), "Only tuple");

    // Advance
    ++it;
    EXPECT_EQ(it, page.end());
    EXPECT_TRUE(it.IsEnd());
    EXPECT_FALSE(it.HasNext());

    // Calling ++ on end iterator is safe and remains at end
    ++it;
    EXPECT_EQ(it, page.end());
}

TEST_F(TablePageTest, IteratorMultipleLiveTuples) {
    TablePage page;
    page.Init(7);

    std::vector<std::string> payloads = {
        "First item",
        "Second item - medium length text",
        "Third item",
        "Fourth item - longer content with numbers 1234567890",
        "Fifth item"
    };

    std::vector<RID> rids;
    for (const auto &p : payloads) {
        RID r;
        ASSERT_TRUE(page.InsertTuple(Tuple(p), &r));
        rids.push_back(r);
    }

    // Iterate using standard range-for
    std::vector<std::string> collected;
    for (const auto &tuple : page) {
        collected.emplace_back(tuple.AsStringView());
    }
    EXPECT_EQ(collected, payloads);

    // Iterate using STL vector constructor from iterator range
    std::vector<Tuple> tuple_vec(page.begin(), page.end());
    ASSERT_EQ(tuple_vec.size(), payloads.size());
    for (size_t i = 0; i < payloads.size(); ++i) {
        EXPECT_EQ(tuple_vec[i].AsStringView(), payloads[i]);
        EXPECT_EQ(tuple_vec[i].GetRid(), rids[i]);
    }

    // Verify slot indices
    size_t idx = 0;
    for (auto it = page.Begin(); it != page.End(); ++it, ++idx) {
        EXPECT_EQ(it.GetSlotNum(), idx);
        EXPECT_EQ(it.GetRid(), rids[idx]);
    }
}

TEST_F(TablePageTest, IteratorSkipMarkDeletedTuples) {
    TablePage page;
    page.Init(1);

    std::vector<RID> rids(6);
    ASSERT_TRUE(page.InsertTuple(Tuple("T0_live"), &rids[0]));
    ASSERT_TRUE(page.InsertTuple(Tuple("T1_delete"), &rids[1]));
    ASSERT_TRUE(page.InsertTuple(Tuple("T2_live"), &rids[2]));
    ASSERT_TRUE(page.InsertTuple(Tuple("T3_delete"), &rids[3]));
    ASSERT_TRUE(page.InsertTuple(Tuple("T4_live"), &rids[4]));
    ASSERT_TRUE(page.InsertTuple(Tuple("T5_delete"), &rids[5]));

    // Mark slots 1, 3, 5 as deleted
    EXPECT_TRUE(page.MarkDelete(rids[1]));
    EXPECT_TRUE(page.MarkDelete(rids[3]));
    EXPECT_TRUE(page.MarkDelete(rids[5]));

    // Iterate - should only visit live slots (0, 2, 4)
    std::vector<std::pair<uint32_t, std::string>> visited;
    for (auto it = page.begin(); it != page.end(); ++it) {
        visited.emplace_back(it.GetSlotNum(), std::string(it->AsStringView()));
    }

    ASSERT_EQ(visited.size(), 3);
    EXPECT_EQ(visited[0].first, 0);
    EXPECT_EQ(visited[0].second, "T0_live");

    EXPECT_EQ(visited[1].first, 2);
    EXPECT_EQ(visited[1].second, "T2_live");

    EXPECT_EQ(visited[2].first, 4);
    EXPECT_EQ(visited[2].second, "T4_live");
}

TEST_F(TablePageTest, IteratorSkipFirstSlotDeleted) {
    TablePage page;
    page.Init(1);

    RID r0, r1, r2;
    ASSERT_TRUE(page.InsertTuple(Tuple("T0_dead"), &r0));
    ASSERT_TRUE(page.InsertTuple(Tuple("T1_live"), &r1));
    ASSERT_TRUE(page.InsertTuple(Tuple("T2_live"), &r2));

    EXPECT_TRUE(page.MarkDelete(r0));

    // begin() should skip slot 0 and immediately point to slot 1
    auto it = page.begin();
    ASSERT_NE(it, page.end());
    EXPECT_EQ(it.GetSlotNum(), 1);
    EXPECT_EQ(it->AsStringView(), "T1_live");

    ++it;
    ASSERT_NE(it, page.end());
    EXPECT_EQ(it.GetSlotNum(), 2);
    EXPECT_EQ(it->AsStringView(), "T2_live");

    ++it;
    EXPECT_EQ(it, page.end());
}

TEST_F(TablePageTest, IteratorSkipApplyDeletedAndReusedSlots) {
    TablePage page;
    page.Init(1);

    RID r0, r1, r2;
    ASSERT_TRUE(page.InsertTuple(Tuple("Alpha"), &r0));
    ASSERT_TRUE(page.InsertTuple(Tuple("Beta"), &r1));
    ASSERT_TRUE(page.InsertTuple(Tuple("Gamma"), &r2));

    // ApplyDelete physically clears offset and size of slot 1
    EXPECT_TRUE(page.ApplyDelete(r1));

    // Iteration skips slot 1
    std::vector<std::string> collected;
    for (const auto &t : page) {
        collected.emplace_back(t.AsStringView());
    }
    EXPECT_EQ(collected, (std::vector<std::string>{"Alpha", "Gamma"}));

    // Reuse slot 1 with a new tuple
    RID r_reuse;
    ASSERT_TRUE(page.InsertTuple(Tuple("Beta_Reused"), &r_reuse));
    EXPECT_EQ(r_reuse.GetSlotNum(), 1);

    // Now all 3 slots (0, 1, 2) should be visited in slot order
    collected.clear();
    for (const auto &t : page) {
        collected.emplace_back(t.AsStringView());
    }
    EXPECT_EQ(collected, (std::vector<std::string>{"Alpha", "Beta_Reused", "Gamma"}));
}

TEST_F(TablePageTest, IteratorSkipAllDeleted) {
    TablePage page;
    page.Init(2);

    RID r0, r1, r2;
    ASSERT_TRUE(page.InsertTuple(Tuple("One"), &r0));
    ASSERT_TRUE(page.InsertTuple(Tuple("Two"), &r1));
    ASSERT_TRUE(page.InsertTuple(Tuple("Three"), &r2));

    EXPECT_TRUE(page.MarkDelete(r0));
    EXPECT_TRUE(page.MarkDelete(r1));
    EXPECT_TRUE(page.MarkDelete(r2));

    EXPECT_EQ(page.begin(), page.end());
    EXPECT_TRUE(page.begin().IsEnd());

    int count = 0;
    for (const auto &t : page) {
        (void)t;
        count++;
    }
    EXPECT_EQ(count, 0);
}

TEST_F(TablePageTest, IteratorRollbackDelete) {
    TablePage page;
    page.Init(3);

    RID r0, r1;
    ASSERT_TRUE(page.InsertTuple(Tuple("Preserved"), &r0));
    ASSERT_TRUE(page.InsertTuple(Tuple("Temporary"), &r1));

    EXPECT_TRUE(page.MarkDelete(r1));

    // Currently only r0 is visited
    std::vector<std::string> collected;
    for (const auto &t : page) {
        collected.emplace_back(t.AsStringView());
    }
    EXPECT_EQ(collected, (std::vector<std::string>{"Preserved"}));

    // Rollback delete on r1
    EXPECT_TRUE(page.RollbackDelete(r1));

    // Now both r0 and r1 are visited
    collected.clear();
    for (const auto &t : page) {
        collected.emplace_back(t.AsStringView());
    }
    EXPECT_EQ(collected, (std::vector<std::string>{"Preserved", "Temporary"}));
}

TEST_F(TablePageTest, IteratorPostIncrement) {
    TablePage page;
    page.Init(1);

    RID r0, r1;
    ASSERT_TRUE(page.InsertTuple(Tuple("A"), &r0));
    ASSERT_TRUE(page.InsertTuple(Tuple("B"), &r1));

    auto it = page.begin();
    auto prev = it++;

    EXPECT_EQ(prev.GetSlotNum(), 0);
    EXPECT_EQ(prev->AsStringView(), "A");

    EXPECT_EQ(it.GetSlotNum(), 1);
    EXPECT_EQ(it->AsStringView(), "B");

    prev = it++;
    EXPECT_EQ(prev.GetSlotNum(), 1);
    EXPECT_EQ(prev->AsStringView(), "B");

    EXPECT_EQ(it, page.end());
}

TEST_F(TablePageTest, IteratorNextMethod) {
    TablePage page;
    page.Init(5);

    RID r0, r1, r2, r3;
    ASSERT_TRUE(page.InsertTuple(Tuple("Val 0"), &r0));
    ASSERT_TRUE(page.InsertTuple(Tuple("Val 1 (del)"), &r1));
    ASSERT_TRUE(page.InsertTuple(Tuple("Val 2"), &r2));
    ASSERT_TRUE(page.InsertTuple(Tuple("Val 3 (del)"), &r3));

    page.MarkDelete(r1);
    page.MarkDelete(r3);

    auto it = page.begin();
    Tuple tuple;
    RID rid;

    std::vector<std::pair<RID, std::string>> results;
    while (it.Next(&tuple, &rid)) {
        results.emplace_back(rid, std::string(tuple.AsStringView()));
    }

    ASSERT_EQ(results.size(), 2);
    EXPECT_EQ(results[0].first, r0);
    EXPECT_EQ(results[0].second, "Val 0");

    EXPECT_EQ(results[1].first, r2);
    EXPECT_EQ(results[1].second, "Val 2");

    EXPECT_FALSE(it.Next(&tuple, &rid));
}

TEST_F(TablePageTest, IteratorStartFromSlotOrRid) {
    TablePage page;
    page.Init(10);

    RID r0, r1, r2, r3, r4;
    ASSERT_TRUE(page.InsertTuple(Tuple("S0"), &r0));
    ASSERT_TRUE(page.InsertTuple(Tuple("S1"), &r1));
    ASSERT_TRUE(page.InsertTuple(Tuple("S2"), &r2));
    ASSERT_TRUE(page.InsertTuple(Tuple("S3"), &r3));
    ASSERT_TRUE(page.InsertTuple(Tuple("S4"), &r4));

    // Start from slot 2
    auto it_slot2 = page.Begin(2);
    EXPECT_EQ(it_slot2.GetSlotNum(), 2);
    EXPECT_EQ(it_slot2->AsStringView(), "S2");

    // Start from RID of slot 3
    auto it_rid3 = page.Begin(r3);
    EXPECT_EQ(it_rid3.GetSlotNum(), 3);
    EXPECT_EQ(it_rid3->AsStringView(), "S3");

    // Start from out-of-bounds slot
    auto it_oob = page.Begin(100);
    EXPECT_EQ(it_oob, page.end());
    EXPECT_TRUE(it_oob.IsEnd());

    // Start from RID of different page
    auto it_diff_page = page.Begin(RID(999, 1));
    EXPECT_EQ(it_diff_page, page.end());
}

TEST_F(TablePageTest, IteratorWithDefragmentation) {
    TablePage page;
    page.Init(1);

    RID r0, r1, r2, r3;
    ASSERT_TRUE(page.InsertTuple(Tuple("Keep 0"), &r0));
    ASSERT_TRUE(page.InsertTuple(Tuple(std::string(200, 'X')), &r1));
    ASSERT_TRUE(page.InsertTuple(Tuple("Keep 2"), &r2));
    ASSERT_TRUE(page.InsertTuple(Tuple(std::string(300, 'Y')), &r3));

    EXPECT_TRUE(page.ApplyDelete(r1));
    EXPECT_TRUE(page.ApplyDelete(r3));

    // Defragment compacts page
    page.Defragment();

    // Verify iterator visits remaining live tuples
    std::vector<std::string> collected;
    for (const auto &t : page) {
        collected.emplace_back(t.AsStringView());
    }
    EXPECT_EQ(collected, (std::vector<std::string>{"Keep 0", "Keep 2"}));
}

TEST_F(TablePageTest, GetTupleOverloads) {
    TablePage page;
    page.Init(42);

    RID r0, r1, r2;
    ASSERT_TRUE(page.InsertTuple(Tuple("Tuple zero"), &r0));
    ASSERT_TRUE(page.InsertTuple(Tuple("Tuple one"), &r1));
    ASSERT_TRUE(page.InsertTuple(Tuple("Tuple two"), &r2));

    EXPECT_TRUE(page.MarkDelete(r1));

    // 1. GetTuple(slot_id, Tuple*)
    Tuple t_out;
    EXPECT_TRUE(page.GetTuple(0, &t_out));
    EXPECT_EQ(t_out.AsStringView(), "Tuple zero");
    EXPECT_EQ(t_out.GetRid(), r0);

    EXPECT_FALSE(page.GetTuple(1, &t_out)); // Deleted
    EXPECT_FALSE(page.GetTuple(99, &t_out)); // Out of bounds

    EXPECT_TRUE(page.GetTuple(2, &t_out));
    EXPECT_EQ(t_out.AsStringView(), "Tuple two");
    EXPECT_EQ(t_out.GetRid(), r2);

    // 2. GetTuple(slot_id) -> std::optional<Tuple>
    auto opt0 = page.GetTuple(0);
    ASSERT_TRUE(opt0.has_value());
    EXPECT_EQ(opt0->AsStringView(), "Tuple zero");

    auto opt1 = page.GetTuple(1);
    EXPECT_FALSE(opt1.has_value()); // Deleted

    auto opt99 = page.GetTuple(99);
    EXPECT_FALSE(opt99.has_value()); // Out of bounds

    // 3. GetTuple(rid) -> std::optional<Tuple>
    auto opt_rid0 = page.GetTuple(r0);
    ASSERT_TRUE(opt_rid0.has_value());
    EXPECT_EQ(opt_rid0->AsStringView(), "Tuple zero");

    auto opt_rid1 = page.GetTuple(r1);
    EXPECT_FALSE(opt_rid1.has_value()); // Deleted

    auto opt_diff_page = page.GetTuple(RID(999, 0));
    EXPECT_FALSE(opt_diff_page.has_value()); // Wrong page
}

TEST_F(TablePageTest, IteratorStandardConceptsAndSTL) {
    // Verify TablePageIterator satisfies std::forward_iterator concept
    static_assert(std::forward_iterator<TablePageIterator>);

    TablePage page;
    page.Init(1);

    ASSERT_TRUE(page.InsertTuple(Tuple("Apple"), nullptr));
    ASSERT_TRUE(page.InsertTuple(Tuple("Banana"), nullptr));
    ASSERT_TRUE(page.InsertTuple(Tuple("Apricot"), nullptr));
    ASSERT_TRUE(page.InsertTuple(Tuple("Avocado"), nullptr));
    ASSERT_TRUE(page.InsertTuple(Tuple("Blueberry"), nullptr));

    // std::distance
    EXPECT_EQ(std::distance(page.begin(), page.end()), 5);

    // std::count_if: count strings starting with 'A'
    auto a_count = std::count_if(page.begin(), page.end(), [](const Tuple &t) {
        return !t.IsEmpty() && t.GetData()[0] == 'A';
    });
    EXPECT_EQ(a_count, 3);

    // std::find_if
    auto it_found = std::find_if(page.begin(), page.end(), [](const Tuple &t) {
        return t.AsStringView() == "Apricot";
    });
    ASSERT_NE(it_found, page.end());
    EXPECT_EQ(it_found.GetSlotNum(), 2);
    EXPECT_EQ(it_found->AsStringView(), "Apricot");

    // std::find_if not found
    auto it_missing = std::find_if(page.begin(), page.end(), [](const Tuple &t) {
        return t.AsStringView() == "Cherry";
    });
    EXPECT_EQ(it_missing, page.end());
}

TEST_F(TablePageTest, IteratorConstPageAccess) {
    TablePage page;
    page.Init(1);

    ASSERT_TRUE(page.InsertTuple(Tuple("Const read 1"), nullptr));
    ASSERT_TRUE(page.InsertTuple(Tuple("Const read 2"), nullptr));

    const TablePage &const_page = page;

    std::vector<std::string> collected;
    for (const auto &t : const_page) {
        collected.emplace_back(t.AsStringView());
    }
    EXPECT_EQ(collected, (std::vector<std::string>{"Const read 1", "Const read 2"}));

    // cbegin / cend
    EXPECT_NE(const_page.cbegin(), const_page.cend());
    EXPECT_EQ(const_page.cbegin()->AsStringView(), "Const read 1");
}


