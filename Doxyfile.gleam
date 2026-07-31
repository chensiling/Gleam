# Doxyfile for the Gleam debugger (run from the GleeBug root directory).
# Usage:  doxygen Doxyfile.gleam

#---------------------------------------------------------------------------
# Project
#---------------------------------------------------------------------------
PROJECT_NAME           = "Gleam"
PROJECT_BRIEF          = "Command-driven headless debugger built on GleeBug"
PROJECT_NUMBER         =
OUTPUT_DIRECTORY       = docs/gleam

#---------------------------------------------------------------------------
# Input
#---------------------------------------------------------------------------
INPUT                  = Gleam
INPUT_ENCODING         = UTF-8
FILE_PATTERNS          = *.cpp *.h *.md
RECURSIVE              = YES
EXCLUDE_PATTERNS       =

#---------------------------------------------------------------------------
# Extraction / parsing
#---------------------------------------------------------------------------
EXTRACT_ALL            = YES
EXTRACT_PRIVATE        = NO
EXTRACT_STATIC         = YES
HIDE_UNDOC_MEMBERS     = NO
BRIEF_MEMBER_DESC      = YES
REPEAT_BRIEF           = YES
# Treat the first sentence of a plain // comment as the brief description.
JAVADOC_AUTOBRIEF      = NO
# /// or //! are already Doxygen; plain // comments are NOT extracted unless
# EXTRACT_ALL is YES (they show as undocumented).
QT_AUTOBRIEF           = NO
MULTILINE_CPP_IS_BRIEF = NO

#---------------------------------------------------------------------------
# Output formats
#---------------------------------------------------------------------------
GENERATE_HTML          = YES
HTML_OUTPUT            = html
GENERATE_LATEX         = NO
GENERATE_XML           = NO

#---------------------------------------------------------------------------
# Diagrams
#---------------------------------------------------------------------------
HAVE_DOT               = NO
CLASS_DIAGRAMS         = YES

#---------------------------------------------------------------------------
# Warnings
#---------------------------------------------------------------------------
QUIET                  = NO
WARNINGS               = YES
WARN_IF_UNDOCUMENTED   = YES
WARN_NO_PARAMDOC       = YES
