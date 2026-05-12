# Phase 4: Tool System

## Goal

Implement all ~20 agent tools and the `ToolRegistry` that manages them. Each tool is implemented as a function that takes a `context.Context` and typed parameters, returns `(string, error)`. Tools are then wrapped into a `Tool` struct with JSON schema for function calling.

## Files

| File | Purpose |
|------|---------|
| `tools/tool.go` | `Tool`, `ToolPermission`, `ToolResult` types |
| `tools/registry.go` | `ToolRegistry` — register, serialise, execute |
| `tools/path.go` | `ResolvePath` sandboxing |
| `tools/path_test.go` | Tests for path resolution |
| `tools/filesystem.go` | `list_files`, `project_tree` |
| `tools/file_io.go` | `read_file`, `read_file_lines`, `write_file`, `edit_file`, `delete_file`, `move_file`, `rename_file` |
| `tools/bash.go` | `run_bash` |
| `tools/grep.go` | `grep_files` |
| `tools/web.go` | `web_search`, `web_fetch` |
| `tools/web_test.go` | Tests for web tools (use httptest) |
| `tools/git_helpers.go` | `OpenGitRepo`, `GetCurrentGitBranch`, `StatusChar`, `IsGitignored` |
| `tools/git.go` | `git_status`, `git_diff`, `git_log`, `git_add`, `git_commit` |
| `tools/git_test.go` | Tests for git tools (use git.Repository fixtures) |
| `tools/web_helpers.go` | `HTTPGet`, cache, rate limiter |
| `tools/worktree.go` | `start_worktree`, `stop_worktree` |
| `tools/worktree_test.go` | Tests for worktree tools |
| `tools/plan_tools.go` | `write_plan`, `read_plan`, `comment_plan` (take `*plan.PlanBoard`) |
| `tools/plan_tools_test.go` | Tests for plan tools |
| `tools/registry_test.go` | Tests for ToolRegistry |
| `tools/plan_tools.go` | `write_plan`, `read_plan`, `comment_plan` (take `*plan.PlanBoard`) |
| `tools/plan_tools_test.go` | Tests for plan tools |
| `tools/registry_test.go` | Tests for ToolRegistry |

---

## Step 4.1: tools/tool.go

### Types

```go
package tools

type ToolPermission int
const (
    PermissionReadOnly  ToolPermission = iota
    PermissionWrite
    PermissionInternal
)

type Tool struct {
    Name        string
    Description string
    Parameters  map[string]any   // JSON Schema object
    Permission  ToolPermission
    Timeout     time.Duration    // 0 = no timeout
    Execute     func(ctx context.Context, args map[string]any) (string, error)
}

// ToOpenAIFunction returns the OpenAI function definition.
func (t *Tool) ToOpenAIFunction() map[string]any
```

### Failing Tests

1. **TestToolToOpenAIFunction** — correct JSON structure emitted
2. **TestToolExecute** — basic execution returns correct result
3. **TestToolTimeout** — tool respects timeout set by context

---

## Step 4.2: tools/path.go

### Types

```go
// ResolvePath resolves rawPath against safeDir.
// Returns an error if the resolved path is outside safeDir or any extraAllowed path.
func ResolvePath(rawPath, safeDir string, extraAllowed ...string) (string, error)
```

### Behaviour

- Relative paths are resolved against `safeDir`
- Uses `filepath.Clean` and `filepath.Abs` for normalization
- Rejects paths that escape `safeDir` via `..`
- Extra allowed paths bypass the check (for read-only tools accessing `/usr/include` etc.)

### Failing Tests

1. **TestResolveRelative** — `"foo"` in safeDir → `safeDir/foo`
2. **TestResolveAbsolute** — `"/tmp/foo"` → error if outside safeDir
3. **TestResolveExtraAllowed** — extra allowed path is accepted
4. **TestResolveEscape** — `"../../etc/passwd"` → error
5. **TestResolveEmptyPath** — empty path → error
6. **TestResolveCurrentDir** — `"."` → safeDir
7. **TestResolveSymlinkInside** — symlink within safeDir resolves (if using EvalSymlinks)

---

## Step 4.3: tools/registry.go

### Types

```go
type ToolRegistry struct { ... }

func NewToolRegistry() *ToolRegistry
func (r *ToolRegistry) Add(t Tool)
func (r *ToolRegistry) AddDefaults(safeDir string, opts ...AddDefaultsOption)
func (r *ToolRegistry) ToOpenAITools() []map[string]any
func (r *ToolRegistry) ToOpenAIToolsFiltered(onlyThese map[string]bool) []map[string]any
func (r *ToolRegistry) Execute(ctx context.Context, name, argsJSON string) (string, error)
func (r *ToolRegistry) ToolNamesByPermission(perm ToolPermission) []string
func (r *ToolRegistry) Tools() []Tool
```

### Behaviour

- `AddDefaults` registers all standard tools (filesystem, file_io, bash, grep, web, git, worktree)
- `Execute` parses args JSON then calls the tool, with optional timeout context
- If no such tool → error "unknown tool: {name}"
- If args JSON invalid → error "invalid JSON arguments: {detail}"

### Failing Tests

1. **TestRegistryAddAndFind** — tool added, found by name
2. **TestRegistryExecute** — tool executes correctly
3. **TestRegistryExecuteUnknown** — returns error for unknown tool
4. **TestRegistryExecuteInvalidJSON** — returns error for bad JSON args
5. **TestRegistryToOpenAITools** — all tools serialized to function-calling format
6. **TestRegistryToOpenAIToolsFiltered** — only selected tools included
7. **TestRegistryToolNamesByPermission** — correct names returned per permission
8. **TestRegistryExecuteTimeout** — tool cancelled by deadline
9. **TestRegistryAddDefaultsCount** — at least 20 tools registered
10. **TestRegistryAddDefaultsIncludesPlanTools** — write_plan etc. present

---

## Step 4.4: tools/filesystem.go

### Tools

1. **`list_files`**: list files/directories at path (like `ls`)
2. **`project_tree`**: recursive tree listing (max depth 5, max lines 500, skip .git)

### Failing Tests

1. **TestListFiles** — lists contents of a temp directory
2. **TestListFilesOutsideSafeDir** — error for path outside safe dir
3. **TestListFilesNotADirectory** — error for file path
4. **TestProjectTreeBasic** — tree output format for a temp directory
5. **TestProjectTreeMaxDepth** — depth limit enforced
6. **TestProjectTreeMaxLines** — line count capped at 500
7. **TestProjectTreeSkipsGit** — .git directory skipped
8. **TestProjectTreeEmptyDir** — "(empty directory)" output

---

## Step 4.5: tools/file_io.go

### Tools

1. **`read_file`**: read N lines from offset (max 400 lines)
2. **`read_file_lines`**: read specific line range, lines prefixed with numbers
3. **`write_file`**: write content, creating parent dirs
4. **`edit_file`**: search-and-replace (must match exactly once)
5. **`delete_file`**: delete a regular file
6. **`move_file`**: move/rename, no overwrite, cross-device copy fallback
7. **`rename_file`**: rename within same directory (basename only)

### Failing Tests

1. **TestReadFile** — read existing file, offset and max_lines
2. **TestReadFileNonexistent** — error for missing file
3. **TestReadFileLines** — line numbers in output
4. **TestReadFileLinesRange** — specific start/end line
5. **TestWriteFile** — write content, verify on disk
6. **TestWriteFileCreatesDirs** — intermediate dirs created
7. **TestEditFileFoundOnce** — exact match replaced
8. **TestEditFileNotFound** — error: 0 matches
9. **TestEditFileMultipleMatches** — error: 2+ matches
10. **TestDeleteFile** — file removed
11. **TestDeleteFileNonexistent** — error
12. **TestMoveFile** — file moved
13. **TestMoveFileOverwrite** — error if destination exists
14. **TestMoveFileCrossDevice** — fallback to copy+delete
15. **TestRenameFile** — rename within directory

---

## Step 4.6: tools/bash.go

### Tool

1. **`run_bash`**: run shell command, stdout+stderr, capped at 500 lines / 16000 chars, 30s timeout

### Failing Tests

1. **TestRunBashEcho** — `echo hello` → `"hello\n"`
2. **TestRunBashExitCode** — non-zero exit included in output
3. **TestRunBashTimeout** — long-running command is killed
4. **TestRunBashOutputCappedLines** — >500 lines truncated
5. **TestRunBashOutputCappedChars** — >16000 chars truncated
6. **TestRunBashCancellation** — cancelled context kills process
7. **TestRunBashEmptyCommand** — error
8. **TestRunBashWorkingDir** — runs in safe_dir
9. **TestRunBashStderr** — stderr captured in output
10. **TestRunBashEnvironment** — inherits env from parent

---

## Step 4.7: tools/grep.go

### Tool

1. **`grep_files`**: regex search (max 200 results), skip .git and gitignored

### Failing Tests

1. **TestGrepFilesBasic** — find pattern in single file
2. **TestGrepFilesRecursive** — find pattern across subdirectories
3. **TestGrepFilesMaxResults** — capped at 200
4. **TestGrepFilesNoMatch** — "(no matches)" output
5. **TestGrepFilesSkipsGit** — .git directory not searched
6. **TestGrepFilesRegexError** — bad regex returns error
7. **TestGrepFilesCancellation** — cancelled context stops search
8. **TestGrepFilesPathOutsideSafeDir** — error

---

## Step 4.8: tools/web.go

### Tools

1. **`web_search`**: DuckDuckGo / Google CSE / custom endpoint
2. **`web_fetch`**: HTTP GET with caching (max 100K chars)

### Failing Tests

1. **TestWebSearchDDG** — uses httptest to mock DuckDuckGo API response
2. **TestWebSearchGoogle** — uses httptest to mock Google CSE response
3. **TestWebSearchCustom** — custom endpoint with {query} placeholder
4. **TestWebSearchEmptyQuery** — error
5. **TestWebFetchBasic** — fetch text content
6. **TestWebFetchCache** — second fetch returns cached result
7. **TestWebFetchMaxChars** — truncated at 100K
8. **TestWebFetchContentType** — unsupported Content-Type rejected
9. **TestWebFetchInvalidURL** — non-http/https scheme rejected
10. **TestWebFetchTimeout** — slow server times out

---

## Step 4.9: tools/git.go + git_helpers.go

### Tools

1. **`git_status`**: porcelain v1 status (max 200 entries)
2. **`git_diff`**: unified diff (max 500 lines / 16000 chars)
3. **`git_log`**: commit history (max 50 commits)
4. **`git_add`**: stage file(s)
5. **`git_commit`**: commit staged changes

### Helpers

- `OpenGitRepo(path string) (*Repository, error)` — wrapper around go-git
- `GetCurrentGitBranch(repoPath string) (string, error)`
- `StatusCharForIndex(flags) byte`
- `StatusCharForWorkdir(flags) byte`
- `IsGitignored(repo, absPath, workdir string) bool`

### Failing Tests (use `go-git` to create temporary repos)

1. **TestGitStatusClean** — empty repo → "(clean — no changes)"
2. **TestGitStatusModified** — modify file → shows M
3. **TestGitStatusUntracked** — new file → shows ?
4. **TestGitDiffUnstaged** — diff of working tree vs index
5. **TestGitDiffStaged** — diff of index vs HEAD
6. **TestGitDiffCapped** — truncated at limits
7. **TestGitLogBasic** — commit history
8. **TestGitLogFormatOneline** — oneline format
9. **TestGitLogWithBranch** — specific branch
10. **TestGitLogMaxCount** — limited to N commits
11. **TestGitAdd** — file staged
12. **TestGitCommit** — commit created
13. **TestGitCommitAll** — `--all` style commit
14. **TestGetCurrentGitBranch** — branch name returned
15. **TestOpenGitRepoNotARepo** — error for non-repo path
16. **TestIsGitignored** — .gitignore respected
17. **TestGitStatusMaxEntries** — capped at 200

---

## Step 4.10: tools/worktree.go

### Tools

1. **`start_worktree`**: create a git worktree on a branch, switch safe_dir
2. **`stop_worktree`**: clean up worktree, restore safe_dir

### State

```go
type WorktreeState struct {
    OriginalSafeDir string
    WorktreeName    string
    WorktreePath    string
    BranchName      string
    Active          bool
}
```

### Failing Tests

1. **TestStartWorktree** — create worktree, verify directory exists
2. **TestStartWorktreeBranchExists** — existing branch checked out
3. **TestStartWorktreeBranchNew** — new branch created from HEAD
4. **TestStopWorktree** — cleanup removes directory
5. **TestStopWorktreeForceDirty** — force:true discards changes
6. **TestStopWorktreeRefusesDirty** — uncommitted changes → error without force
7. **TestStopWorktreeRefusesUnmerged** — unmerged branch → error without force
8. **TestStartWorktreeAlreadyActive** — calling twice while active → error
9. **TestStopWorktreeNoActive** — calling without active → error
10. **TestWorktreeSafeDirSwitching** — safe_dir points to worktree after start, restored after stop
11. **TestWorktreeRemoveAllSafe** — utility function for safe recursive deletion

---

## Step 4.11: tools/plan_tools.go

### Tools

1. **`write_plan`**: replaces plan body
2. **`read_plan`**: returns plan body + comments
3. **`comment_plan`**: appends a comment

Each tool takes a `*plan.PlanBoard` reference during construction.

### Failing Tests

1. **TestWritePlanTool** — writes plan, read_plan reflects it
2. **TestReadPlanTool** — returns "(empty plan)" for fresh board
3. **TestCommentPlanTool** — appends comment, read_plan includes it
4. **TestPlanToolErrors** — empty comment returns error
5. **TestPlanToolsInRegistry** — plan tools included in AddDefaults

---

## Running Phase 4 Tests

```bash
cd go
go test ./tools/...
```
