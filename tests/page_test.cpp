#include <gtest/gtest.h>
#include "page.h"
#include <cstring>

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