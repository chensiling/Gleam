// Command history and completion implementation

#include "History.h"
#include <fstream>
#include <algorithm>
#include <iostream>
#include <conio.h>  // For _getch() on Windows
#include <cstdio>

namespace Gleam {

// CommandHistory implementation

CommandHistory::CommandHistory(size_t maxSize)
    : mMaxSize(maxSize), mCurrentIndex(0) {
}

void CommandHistory::add(const std::string& command) {
    // Don't add empty commands
    if (command.empty())
        return;

    // Don't add duplicate of last command
    if (!mHistory.empty() && mHistory.back() == command)
        return;

    mHistory.push_back(command);

    // Trim if exceeds max size
    while (mHistory.size() > mMaxSize) {
        mHistory.pop_front();
    }

    // Reset navigation
    resetNavigation();
}

std::string CommandHistory::navigateUp() {
    if (mHistory.empty())
        return "";

    if (mCurrentIndex == mHistory.size()) {
        // First up press - save current line
        mCurrentIndex--;
    } else if (mCurrentIndex > 0) {
        mCurrentIndex--;
    }

    return mHistory[mCurrentIndex];
}

std::string CommandHistory::navigateDown() {
    if (mHistory.empty())
        return "";

    if (mCurrentIndex < mHistory.size() - 1) {
        mCurrentIndex++;
        return mHistory[mCurrentIndex];
    } else {
        // At end - return to current line
        mCurrentIndex = mHistory.size();
        return mCurrentLine;
    }
}

void CommandHistory::resetNavigation() {
    mCurrentIndex = mHistory.size();
    mCurrentLine.clear();
}

bool CommandHistory::loadFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open())
        return false;

    mHistory.clear();
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            mHistory.push_back(line);
        }
    }

    // Trim to max size
    while (mHistory.size() > mMaxSize) {
        mHistory.pop_front();
    }

    resetNavigation();
    return true;
}

bool CommandHistory::saveToFile(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open())
        return false;

    for (const auto& cmd : mHistory) {
        file << cmd << '\n';
    }

    return true;
}

void CommandHistory::clear() {
    mHistory.clear();
    resetNavigation();
}

std::vector<std::string> CommandHistory::search(const std::string& prefix) const {
    std::vector<std::string> matches;

    for (auto it = mHistory.rbegin(); it != mHistory.rend(); ++it) {
        if (it->find(prefix) == 0) {  // Starts with prefix
            matches.push_back(*it);
        }
    }

    return matches;
}

// CommandCompleter implementation

CommandCompleter::CommandCompleter() : mMatchIndex(0) {
}

void CommandCompleter::setCommands(const std::vector<std::string>& commands) {
    mCommands = commands;
    std::sort(mCommands.begin(), mCommands.end());
}

void CommandCompleter::addCommand(const std::string& command) {
    mCommands.push_back(command);
    std::sort(mCommands.begin(), mCommands.end());
}

std::string CommandCompleter::complete(const std::string& partial) {
    mCurrentMatches = getMatches(partial);
    mMatchIndex = 0;

    if (mCurrentMatches.empty())
        return partial;

    if (mCurrentMatches.size() == 1)
        return mCurrentMatches[0];

    // Multiple matches - return first
    return mCurrentMatches[0];
}

std::string CommandCompleter::nextMatch() {
    if (mCurrentMatches.empty())
        return "";

    mMatchIndex = (mMatchIndex + 1) % mCurrentMatches.size();
    return mCurrentMatches[mMatchIndex];
}

void CommandCompleter::resetCompletion() {
    mCurrentMatches.clear();
    mMatchIndex = 0;
}

std::vector<std::string> CommandCompleter::getMatches(const std::string& partial) const {
    std::vector<std::string> matches;

    for (const auto& cmd : mCommands) {
        if (cmd.find(partial) == 0) {  // Starts with partial
            matches.push_back(cmd);
        }
    }

    return matches;
}

// LineEditor implementation

LineEditor::LineEditor() : mPrompt("> "), mEnabled(false) {
}

std::string LineEditor::readLine() {
    // For now, use simple getline
    // Full implementation with arrow keys would require platform-specific console handling

    printf("%s", mPrompt.c_str());
    fflush(stdout);

    std::string line;
    std::getline(std::cin, line);

    if (!line.empty()) {
        mHistory.add(line);

        // Auto-save if history file is set via getHistoryFile()
        std::string histFile = mHistory.getHistoryFile();
        if (!histFile.empty()) {
            mHistory.saveToFile(histFile);
        }
    }

    return line;
}

void LineEditor::setHistoryFile(const std::string& filename) {
    mHistory.setHistoryFile(filename);
    mHistory.loadFromFile(filename);
}

void LineEditor::setCommands(const std::vector<std::string>& commands) {
    mCompleter.setCommands(commands);
}

} // namespace Gleam
