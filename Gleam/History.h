// Command history and completion for Gleam REPL

#ifndef GLEAM_HISTORY_H
#define GLEAM_HISTORY_H

#include <string>
#include <vector>
#include <deque>

namespace Gleam {

// Command history manager
class CommandHistory {
private:
    std::deque<std::string> mHistory;
    size_t mMaxSize;
    size_t mCurrentIndex;  // For navigation
    std::string mCurrentLine;  // Temporary storage while navigating
    std::string mHistoryFile;

public:
    CommandHistory(size_t maxSize = 1000);

    // Add command to history
    void add(const std::string& command);

    // Navigation (for arrow keys)
    std::string navigateUp();    // Return previous command
    std::string navigateDown();  // Return next command
    void resetNavigation();      // Reset to end of history

    // Get history
    const std::deque<std::string>& getHistory() const { return mHistory; }
    size_t size() const { return mHistory.size(); }
    bool empty() const { return mHistory.empty(); }

    // Persistence
    bool loadFromFile(const std::string& filename);
    bool saveToFile(const std::string& filename);
    void setHistoryFile(const std::string& filename) { mHistoryFile = filename; }
    const std::string& getHistoryFile() const { return mHistoryFile; }

    // Clear history
    void clear();

    // Search history (for Ctrl+R style search)
    std::vector<std::string> search(const std::string& prefix) const;
};

// Tab completion helper
class CommandCompleter {
private:
    std::vector<std::string> mCommands;
    std::vector<std::string> mCurrentMatches;
    size_t mMatchIndex;

public:
    CommandCompleter();

    // Set available commands
    void setCommands(const std::vector<std::string>& commands);
    void addCommand(const std::string& command);

    // Tab completion
    std::string complete(const std::string& partial);
    std::string nextMatch();  // Cycle through matches
    void resetCompletion();

    // Get all matches
    std::vector<std::string> getMatches(const std::string& partial) const;
};

// Simple line editor with history and completion
class LineEditor {
private:
    CommandHistory mHistory;
    CommandCompleter mCompleter;
    std::string mPrompt;
    bool mEnabled;

public:
    LineEditor();

    // Read line with history and completion support
    std::string readLine();

    // Configuration
    void setPrompt(const std::string& prompt) { mPrompt = prompt; }
    void setHistoryFile(const std::string& filename);
    void setCommands(const std::vector<std::string>& commands);
    void enable(bool enabled) { mEnabled = enabled; }

    // Access history
    CommandHistory& history() { return mHistory; }
    const CommandHistory& history() const { return mHistory; }
};

} // namespace Gleam

#endif // GLEAM_HISTORY_H
