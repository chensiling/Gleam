/**
 * @file History.h
 * @brief Command history and tab completion for the Gleam REPL
 *
 * Provides interactive command-line features:
 * - Command history with up/down arrow navigation
 * - Prefix-based history search (Ctrl+R style)
 * - Tab completion with cycling through matches
 * - Persistent history saved to disk
 * - Simple line editor combining history and completion
 *
 * **Design rationale:**
 * A debugger REPL without history and completion is painful to use.
 * These classes provide the essential UX features expected in modern
 * command-line interfaces while remaining lightweight and focused.
 *
 * @example Basic usage:
 * @code
 * LineEditor editor;
 * editor.setPrompt("gleam> ");
 * editor.setHistoryFile("gleam_history.txt");
 * editor.setCommands({"attach", "breakpoint", "continue", "step", "detach"});
 *
 * while (true) {
 *     std::string line = editor.readLine();
 *     if (line == "quit") break;
 *     processCommand(line);
 * }
 * @endcode
 */

#ifndef GLEAM_HISTORY_H
#define GLEAM_HISTORY_H

#include <string>
#include <vector>
#include <deque>

namespace Gleam {

/**
 * @brief Command history manager with navigation and persistence
 *
 * Maintains a bounded history of commands with support for:
 * - Up/down arrow navigation through history
 * - Prefix-based searching
 * - Persistence to/from disk
 * - Automatic deduplication of consecutive identical commands
 *
 * @example Basic usage:
 * @code
 * CommandHistory history(1000);  // Max 1000 entries
 *
 * history.add("attach 1234");
 * history.add("breakpoint kernel32!CreateFileW");
 * history.add("continue");
 *
 * // Navigate backward
 * std::string prev = history.navigateUp();  // Returns "continue"
 * prev = history.navigateUp();              // Returns "breakpoint..."
 *
 * // Save for next session
 * history.saveToFile("gleam_history.txt");
 * @endcode
 *
 * @example Prefix search:
 * @code
 * history.add("breakpoint kernel32!CreateFileW");
 * history.add("breakpoint ntdll!NtCreateFile");
 * history.add("continue");
 *
 * auto matches = history.search("break");
 * // Returns both breakpoint commands
 * @endcode
 */
class CommandHistory {
private:
    std::deque<std::string> mHistory;     ///< Command history (newest at back)
    size_t mMaxSize;                      ///< Maximum history entries
    size_t mCurrentIndex;                 ///< Current position during navigation
    std::string mCurrentLine;             ///< Temporary storage while navigating
    std::string mHistoryFile;             ///< Path to persistent history file

public:
    /**
     * @brief Construct a command history with size limit
     * @param maxSize Maximum number of history entries (default: 1000)
     *
     * @note Oldest entries are automatically removed when limit is reached
     */
    CommandHistory(size_t maxSize = 1000);

    /**
     * @brief Add a command to history
     *
     * Adds the command to the end of the history. Skips adding if:
     * - The command is empty
     * - The command is identical to the previous entry (deduplication)
     *
     * @param command Command string to add
     *
     * @example
     * @code
     * history.add("attach 1234");
     * history.add("attach 1234");  // Skipped (duplicate)
     * history.add("continue");     // Added
     * @endcode
     */
    void add(const std::string& command);

    /**
     * @brief Navigate to previous command (up arrow)
     *
     * Returns the previous command in history. Repeated calls walk
     * backward through history.
     *
     * @return Previous command, or current line if at beginning
     *
     * @note Call resetNavigation() when the user starts typing a new command
     */
    std::string navigateUp();

    /**
     * @brief Navigate to next command (down arrow)
     *
     * Returns the next command in history. At the end of history,
     * returns the original line being typed.
     *
     * @return Next command, or original line if at end
     */
    std::string navigateDown();

    /**
     * @brief Reset navigation to end of history
     *
     * Call this when the user starts typing a new command or when
     * a command is submitted. Resets the navigation pointer to the
     * end of history.
     */
    void resetNavigation();

    /**
     * @brief Get the entire history
     * @return Reference to the history deque (newest at back)
     */
    const std::deque<std::string>& getHistory() const { return mHistory; }

    /**
     * @brief Get the number of history entries
     * @return Current history size
     */
    size_t size() const { return mHistory.size(); }

    /**
     * @brief Check if history is empty
     * @return true if no commands in history
     */
    bool empty() const { return mHistory.empty(); }

    /**
     * @brief Load history from file
     *
     * Reads history entries from a file (one command per line).
     * Existing in-memory history is replaced.
     *
     * @param filename Path to history file
     * @return true if file was successfully loaded
     *
     * @note Silently fails (returns false) if file doesn't exist
     */
    bool loadFromFile(const std::string& filename);

    /**
     * @brief Save history to file
     *
     * Writes all history entries to a file (one command per line).
     * Creates the file if it doesn't exist.
     *
     * @param filename Path to history file
     * @return true if file was successfully saved
     */
    bool saveToFile(const std::string& filename);

    /**
     * @brief Set the default history file path
     *
     * Used by LineEditor for automatic save on exit.
     *
     * @param filename Path to history file
     */
    void setHistoryFile(const std::string& filename) { mHistoryFile = filename; }

    /**
     * @brief Get the default history file path
     * @return Path to history file (empty if not set)
     */
    const std::string& getHistoryFile() const { return mHistoryFile; }

    /**
     * @brief Clear all history entries
     *
     * Removes all commands from history. Does not delete the history file.
     */
    void clear();

    /**
     * @brief Search history by prefix (Ctrl+R style)
     *
     * Returns all commands that start with the given prefix, in
     * reverse chronological order (newest first).
     *
     * @param prefix Prefix to search for (case-sensitive)
     * @return Vector of matching commands
     *
     * @example
     * @code
     * auto matches = history.search("break");
     * for (const auto& cmd : matches) {
     *     printf("%s\n", cmd.c_str());
     * }
     * @endcode
     */
    std::vector<std::string> search(const std::string& prefix) const;
};

/**
 * @brief Tab completion helper
 *
 * Provides tab completion functionality with cycling through matches.
 * Given a partial command, suggests completions from a known command list.
 *
 * @example Basic usage:
 * @code
 * CommandCompleter completer;
 * completer.setCommands({"attach", "breakpoint", "continue", "detach"});
 *
 * std::string partial = "bre";
 * std::string match = completer.complete(partial);  // Returns "breakpoint"
 *
 * // If multiple matches, tab again to cycle
 * completer.setCommands({"breakpoint", "break"});
 * std::string m1 = completer.complete("bre");  // Returns "break"
 * std::string m2 = completer.nextMatch();      // Returns "breakpoint"
 * std::string m3 = completer.nextMatch();      // Returns "break" (cycles)
 * @endcode
 */
class CommandCompleter {
private:
    std::vector<std::string> mCommands;       ///< Available commands
    std::vector<std::string> mCurrentMatches; ///< Current completion matches
    size_t mMatchIndex;                       ///< Index for cycling through matches

public:
    /**
     * @brief Construct an empty completer
     */
    CommandCompleter();

    /**
     * @brief Set the available commands
     *
     * Replaces the current command list with a new one.
     *
     * @param commands Vector of command names
     */
    void setCommands(const std::vector<std::string>& commands);

    /**
     * @brief Add a single command to the list
     *
     * @param command Command name to add
     */
    void addCommand(const std::string& command);

    /**
     * @brief Complete a partial command
     *
     * Finds all commands that start with the partial string.
     * If multiple matches exist, returns the first one and prepares
     * for cycling via nextMatch().
     *
     * @param partial Partial command to complete
     * @return Completed command, or partial if no matches
     *
     * @note Case-sensitive matching
     */
    std::string complete(const std::string& partial);

    /**
     * @brief Cycle to next completion match
     *
     * Call this on repeated tab presses to cycle through all matches
     * found by the last complete() call.
     *
     * @return Next matching command (wraps around to first)
     */
    std::string nextMatch();

    /**
     * @brief Reset completion state
     *
     * Call this when the user starts typing or moves the cursor.
     * Clears the current match list.
     */
    void resetCompletion();

    /**
     * @brief Get all matches for a partial command
     *
     * Returns all commands that start with the partial string,
     * sorted alphabetically.
     *
     * @param partial Partial command to match
     * @return Vector of matching commands
     */
    std::vector<std::string> getMatches(const std::string& partial) const;
};

/**
 * @brief Simple line editor with history and completion
 *
 * Combines CommandHistory and CommandCompleter into a single interface
 * suitable for use in a REPL. Handles:
 * - Prompt display
 * - Command input with history (up/down arrows)
 * - Tab completion
 * - Automatic history persistence
 *
 * @example
 * @code
 * LineEditor editor;
 * editor.setPrompt("gleam> ");
 * editor.setHistoryFile(".gleam_history");
 * editor.setCommands({"attach", "breakpoint", "continue", "step"});
 * editor.enable(true);
 *
 * while (running) {
 *     std::string line = editor.readLine();
 *     if (line.empty()) continue;
 *
 *     if (line == "quit") break;
 *     executeCommand(line);
 * }
 * @endcode
 *
 * @note This is a basic implementation. For production use, consider
 *       integrating with libedit, readline, or a platform-specific
 *       console API for proper arrow key handling and line editing.
 */
class LineEditor {
private:
    CommandHistory mHistory;      ///< Command history
    CommandCompleter mCompleter;  ///< Tab completer
    std::string mPrompt;          ///< Prompt string
    bool mEnabled;                ///< Enable/disable editor features

public:
    /**
     * @brief Construct a line editor with default settings
     */
    LineEditor();

    /**
     * @brief Read a line of input with history and completion
     *
     * Displays the prompt and reads user input. Supports:
     * - Up/down arrows for history navigation
     * - Tab for completion
     * - Enter to submit
     *
     * @return The command line entered by the user
     *
     * @note In the basic implementation, arrow keys may not work
     *       depending on terminal capabilities. This is a framework
     *       for integration with a proper line editing library.
     */
    std::string readLine();

    /**
     * @brief Set the prompt string
     * @param prompt Prompt to display (e.g., "gleam> ")
     */
    void setPrompt(const std::string& prompt) { mPrompt = prompt; }

    /**
     * @brief Set the history file for persistence
     *
     * The editor will automatically load history from this file on
     * first use and save to it when commands are added.
     *
     * @param filename Path to history file
     */
    void setHistoryFile(const std::string& filename);

    /**
     * @brief Set the available commands for tab completion
     * @param commands Vector of command names
     */
    void setCommands(const std::vector<std::string>& commands);

    /**
     * @brief Enable or disable editor features
     *
     * When disabled, readLine() falls back to basic input without
     * history or completion.
     *
     * @param enabled true to enable features
     */
    void enable(bool enabled) { mEnabled = enabled; }

    /**
     * @brief Access the command history
     * @return Reference to the CommandHistory
     */
    CommandHistory& history() { return mHistory; }

    /**
     * @brief Const access to command history
     * @return Const reference to the CommandHistory
     */
    const CommandHistory& history() const { return mHistory; }
};

} // namespace Gleam

#endif // GLEAM_HISTORY_H
