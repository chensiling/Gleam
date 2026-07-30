// Unit tests for command history and completion

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <deque>

// Mock CommandHistory for testing
class MockCommandHistory {
private:
    std::deque<std::string> mHistory;
    size_t mMaxSize;
    size_t mCurrentIndex;

public:
    MockCommandHistory(size_t maxSize = 1000)
        : mMaxSize(maxSize), mCurrentIndex(0) {}

    void add(const std::string& command) {
        if (command.empty())
            return;
        if (!mHistory.empty() && mHistory.back() == command)
            return;

        mHistory.push_back(command);
        while (mHistory.size() > mMaxSize)
            mHistory.pop_front();

        mCurrentIndex = mHistory.size();
    }

    std::string navigateUp() {
        if (mHistory.empty())
            return "";
        if (mCurrentIndex > 0)
            mCurrentIndex--;
        return mHistory[mCurrentIndex];
    }

    std::string navigateDown() {
        if (mHistory.empty())
            return "";
        if (mCurrentIndex < mHistory.size() - 1) {
            mCurrentIndex++;
            return mHistory[mCurrentIndex];
        }
        mCurrentIndex = mHistory.size();
        return "";
    }

    void reset() {
        mCurrentIndex = mHistory.size();
    }

    size_t size() const { return mHistory.size(); }
    bool empty() const { return mHistory.empty(); }
};

// History Tests

TEST(HistoryTest, InitiallyEmpty) {
    MockCommandHistory hist;
    EXPECT_TRUE(hist.empty());
    EXPECT_EQ(hist.size(), 0);
}

TEST(HistoryTest, AddCommand) {
    MockCommandHistory hist;
    hist.add("help");
    EXPECT_EQ(hist.size(), 1);
}

TEST(HistoryTest, IgnoreEmptyCommands) {
    MockCommandHistory hist;
    hist.add("");
    EXPECT_EQ(hist.size(), 0);
}

TEST(HistoryTest, IgnoreDuplicateLastCommand) {
    MockCommandHistory hist;
    hist.add("help");
    hist.add("help");
    EXPECT_EQ(hist.size(), 1);
}

TEST(HistoryTest, AllowDuplicateNonLastCommand) {
    MockCommandHistory hist;
    hist.add("help");
    hist.add("quit");
    hist.add("help");
    EXPECT_EQ(hist.size(), 3);
}

TEST(HistoryTest, NavigateUp) {
    MockCommandHistory hist;
    hist.add("command1");
    hist.add("command2");
    hist.add("command3");

    EXPECT_EQ(hist.navigateUp(), "command3");
    EXPECT_EQ(hist.navigateUp(), "command2");
    EXPECT_EQ(hist.navigateUp(), "command1");
}

TEST(HistoryTest, NavigateDown) {
    MockCommandHistory hist;
    hist.add("command1");
    hist.add("command2");
    hist.add("command3");

    hist.navigateUp();
    hist.navigateUp();

    EXPECT_EQ(hist.navigateDown(), "command3");
    EXPECT_EQ(hist.navigateDown(), "");  // Back to current
}

TEST(HistoryTest, NavigateUpAtTop) {
    MockCommandHistory hist;
    hist.add("command1");

    EXPECT_EQ(hist.navigateUp(), "command1");
    EXPECT_EQ(hist.navigateUp(), "command1");  // Stay at top
}

TEST(HistoryTest, NavigateEmpty) {
    MockCommandHistory hist;
    EXPECT_EQ(hist.navigateUp(), "");
    EXPECT_EQ(hist.navigateDown(), "");
}

TEST(HistoryTest, MaxSizeLimit) {
    MockCommandHistory hist(3);
    hist.add("cmd1");
    hist.add("cmd2");
    hist.add("cmd3");
    hist.add("cmd4");

    EXPECT_EQ(hist.size(), 3);  // Only keeps last 3
}

// Completion Tests

class MockCompleter {
private:
    std::vector<std::string> mCommands;

public:
    void setCommands(const std::vector<std::string>& commands) {
        mCommands = commands;
    }

    std::vector<std::string> getMatches(const std::string& partial) const {
        std::vector<std::string> matches;
        for (const auto& cmd : mCommands) {
            if (cmd.find(partial) == 0)
                matches.push_back(cmd);
        }
        return matches;
    }
};

TEST(CompletionTest, NoMatches) {
    MockCompleter comp;
    comp.setCommands({"help", "quit", "run"});

    auto matches = comp.getMatches("xyz");
    EXPECT_TRUE(matches.empty());
}

TEST(CompletionTest, SingleMatch) {
    MockCompleter comp;
    comp.setCommands({"help", "quit", "run"});

    auto matches = comp.getMatches("he");
    EXPECT_EQ(matches.size(), 1);
    EXPECT_EQ(matches[0], "help");
}

TEST(CompletionTest, MultipleMatches) {
    MockCompleter comp;
    comp.setCommands({"breakpoint", "break", "bp"});

    auto matches = comp.getMatches("b");
    EXPECT_EQ(matches.size(), 3);
}

TEST(CompletionTest, ExactMatch) {
    MockCompleter comp;
    comp.setCommands({"help", "quit"});

    auto matches = comp.getMatches("help");
    EXPECT_EQ(matches.size(), 1);
    EXPECT_EQ(matches[0], "help");
}

TEST(CompletionTest, EmptyPartial) {
    MockCompleter comp;
    comp.setCommands({"help", "quit", "run"});

    auto matches = comp.getMatches("");
    EXPECT_EQ(matches.size(), 3);  // All commands match
}

// Integration Tests

TEST(HistoryIntegrationTest, NavigationAfterAdd) {
    MockCommandHistory hist;
    hist.add("cmd1");
    hist.add("cmd2");

    EXPECT_EQ(hist.navigateUp(), "cmd2");

    hist.add("cmd3");
    EXPECT_EQ(hist.navigateUp(), "cmd3");
}

TEST(HistoryIntegrationTest, ResetNavigation) {
    MockCommandHistory hist;
    hist.add("cmd1");
    hist.add("cmd2");

    hist.navigateUp();
    hist.reset();

    EXPECT_EQ(hist.navigateUp(), "cmd2");  // Back to most recent
}
