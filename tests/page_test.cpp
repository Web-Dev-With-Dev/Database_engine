#include <gtest/gtest.h>
#include "page.h"
#include <cstring>
#include <random>

TEST(PageTest, Constructor)
{
    Page page;

    EXPECT_EQ(page.GetPageId(), -1);
    EXPECT_EQ(page.GetPinCount(), 0);
    EXPECT_FALSE(page.IsDirty());
}

TEST(PageTest, PageId)
{
    Page page;

    page.SetPageId(10);

    EXPECT_EQ(page.GetPageId(), 10);
}

TEST(PageTest, DirtyFlag)
{
    Page page;

    page.SetDirty(true);

    EXPECT_TRUE(page.IsDirty());

    page.SetDirty(false);

    EXPECT_FALSE(page.IsDirty());
}

TEST(PageTest, PinCount)
{
    Page page;

    page.Pin();
    page.Pin();

    EXPECT_EQ(page.GetPinCount(), 2);

    page.Unpin();

    EXPECT_EQ(page.GetPinCount(), 1);

    page.Unpin();

    EXPECT_EQ(page.GetPinCount(), 0);
}

TEST(PageTest, UnpinBelowZero)
{
    Page page;

    page.Unpin();

    EXPECT_EQ(page.GetPinCount(), 0);
}

TEST(PageTest, DataWrite)
{
    Page page;

    strcpy(page.GetData(), "Hello Database");

    EXPECT_STREQ(page.GetData(), "Hello Database");
}

TEST(PageTest, DataModification)
{
    Page page;

    page.GetData()[0] = 'A';
    page.GetData()[1] = 'B';
    page.GetData()[2] = 'C';
    page.GetData()[3] = '\0';

    EXPECT_STREQ(page.GetData(), "ABC");
}

TEST(PageTest, ReadLatch)
{
    Page page;

    page.RLatch();
    page.RUnlatch();

    SUCCEED();
}

TEST(PageTest, WriteLatch)
{
    Page page;

    page.WLatch();
    page.WUnlatch();

    SUCCEED();
}

TEST(PageTest, CombinedOperations)
{
    Page page;

    page.SetPageId(5);
    page.SetDirty(true);

    page.Pin();
    page.Pin();
    page.Unpin();

    EXPECT_EQ(page.GetPageId(), 5);
    EXPECT_TRUE(page.IsDirty());
    EXPECT_EQ(page.GetPinCount(), 1);
}

TEST(PageTest, MaxPageId)
{
    Page page;

    page.SetPageId(INT32_MAX);

    EXPECT_EQ(page.GetPageId(), INT32_MAX);
}

TEST(PageTest, MinPageId)
{
    Page page;

    page.SetPageId(INT32_MIN);

    EXPECT_EQ(page.GetPageId(), INT32_MIN);
}

TEST(PageTest, DirtyFlagMultipleChanges)
{
    Page page;

    for (int i = 0; i < 100; i++)
    {
        page.SetDirty(true);
        EXPECT_TRUE(page.IsDirty());

        page.SetDirty(false);
        EXPECT_FALSE(page.IsDirty());
    }
}

TEST(PageTest, LargePinCount)
{
    Page page;

    for (int i = 0; i < 10000; i++)
    {
        page.Pin();
    }

    EXPECT_EQ(page.GetPinCount(), 10000);
}

TEST(PageTest, PinThenUnpinAll)
{
    Page page;

    for (int i = 0; i < 1000; i++)
        page.Pin();

    for (int i = 0; i < 1000; i++)
        page.Unpin();

    EXPECT_EQ(page.GetPinCount(), 0);
}

TEST(PageTest, RepeatedOverUnpin)
{
    Page page;

    for (int i = 0; i < 100; i++)
        page.Unpin();

    EXPECT_EQ(page.GetPinCount(), 0);
}

TEST(PageTest, FullPageWrite)
{
    Page page;

    memset(page.GetData(), 'X', PAGE_SIZE);

    for (size_t i = 0; i < PAGE_SIZE; i++)
    {
        EXPECT_EQ(page.GetData()[i], 'X');
    }
}

TEST(PageTest, BinaryData)
{
    Page page;

    for (size_t i = 0; i < PAGE_SIZE; i++)
    {
        page.GetData()[i] = static_cast<char>(i % 256);
    }

    for (size_t i = 0; i < PAGE_SIZE; i++)
    {
        EXPECT_EQ(
            static_cast<unsigned char>(page.GetData()[i]),
            static_cast<unsigned char>(i % 256));
    }
}

TEST(PageTest, ZeroInitialization)
{
    Page page;

    for (size_t i = 0; i < PAGE_SIZE; i++)
    {
        EXPECT_EQ(page.GetData()[i], 0);
    }
}

TEST(PageTest, NullBytes)
{
    Page page;

    memset(page.GetData(), 0, PAGE_SIZE);

    EXPECT_EQ(memcmp(page.GetData(),
                     std::vector<char>(PAGE_SIZE, 0).data(),
                     PAGE_SIZE), 0);
}

TEST(PageTest, MemcpyPage)
{
    Page page;

    char source[PAGE_SIZE];

    memset(source, 0xAB, PAGE_SIZE);

    memcpy(page.GetData(), source, PAGE_SIZE);

    EXPECT_EQ(memcmp(page.GetData(), source, PAGE_SIZE), 0);
}

TEST(PageTest, MultiplePagesIndependent)
{
    Page p1;
    Page p2;

    strcpy(p1.GetData(), "First");
    strcpy(p2.GetData(), "Second");

    EXPECT_STREQ(p1.GetData(), "First");
    EXPECT_STREQ(p2.GetData(), "Second");
}


TEST(PageTest, MultipleReadLatch)
{
    Page page;

    page.RLatch();
    page.RLatch();

    page.RUnlatch();
    page.RUnlatch();

    SUCCEED();
}

TEST(PageTest, MultipleWriteLatch)
{
    Page page;

    page.WLatch();
    page.WUnlatch();

    page.WLatch();
    page.WUnlatch();

    SUCCEED();
}


TEST(PageTest, StressTest)
{
    Page page;

    for (int i = 0; i < 1000; i++)
    {
        page.Pin();
        page.SetDirty(i % 2);
        page.SetPageId(i);

        page.GetData()[0] = static_cast<char>(i);

        page.Unpin();
    }

    EXPECT_EQ(page.GetPinCount(), 0);
    EXPECT_EQ(page.GetPageId(), 999);
    EXPECT_TRUE(page.IsDirty());
}

TEST(PageTest, PageIsolation)
{
    Page p1;
    Page p2;

    memset(p1.GetData(), 'A', PAGE_SIZE);
    memset(p2.GetData(), 'B', PAGE_SIZE);

    EXPECT_EQ(p1.GetData()[0], 'A');
    EXPECT_EQ(p2.GetData()[0], 'B');
}

TEST(PageTest, LargeString)
{
    Page page;

    memset(page.GetData(), 'C', PAGE_SIZE - 1);
    page.GetData()[PAGE_SIZE - 1] = '\0';

    EXPECT_EQ(strlen(page.GetData()), PAGE_SIZE - 1);
}

TEST(PageTest, RandomPattern)
{
    Page page;

    std::mt19937 gen(42);
    std::uniform_int_distribution<int> dist(0, 255);

    for (size_t i = 0; i < PAGE_SIZE; i++)
    {
        page.GetData()[i] = static_cast<char>(dist(gen));
    }

    SUCCEED();
}

