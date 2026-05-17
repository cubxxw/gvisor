```markdown
# gvisor Development Patterns

> Auto-generated skill from repository analysis

## Overview
This skill teaches you the core development patterns and conventions used in the `gvisor` repository, a Go-based project. You'll learn about file organization, import/export styles, commit message patterns, and how to write and run tests. This guide also provides suggested commands for common workflows to streamline your development process.

## Coding Conventions

### File Naming
- Use **snake_case** for all file names.
  - Example: `gofer_server.go`, `file_system_test.go`

### Imports
- Use **relative imports** within the project.
  - Example:
    ```go
    import (
        "context"
        "gvisor.dev/gvisor/pkg/sentry/fs"
    )
    ```

### Exports
- Use **named exports** for functions, types, and variables that should be accessible outside the package.
  - Example:
    ```go
    // Exported function
    func NewGoferServer() *GoferServer {
        // ...
    }
    ```

### Commit Messages
- Freeform style, often with a prefix (e.g., `gofer:`).
- Average commit message length: ~37 characters.
  - Example: `gofer: fix race in file descriptor handling`

## Workflows

### Code Contribution
**Trigger:** When adding new features or fixing bugs  
**Command:** `/contribute`

1. Create a new branch for your changes.
2. Follow the snake_case naming convention for new files.
3. Use relative imports for internal packages.
4. Write named exports for any public APIs.
5. Write or update tests in `*_test.go` files.
6. Commit changes with a descriptive message, optionally prefixed (e.g., `gofer:`).
7. Submit a pull request for review.

### Running Tests
**Trigger:** After making code changes  
**Command:** `/test`

1. Ensure all test files are named with the `*_test.go` pattern.
2. Run tests using Go's built-in test tool:
    ```sh
    go test ./...
    ```
3. Review test output and fix any failing tests.

### Reviewing Code Style
**Trigger:** Before submitting code for review  
**Command:** `/lint`

1. Check that all files use snake_case naming.
2. Ensure imports are relative and grouped logically.
3. Verify that exported functions/types use proper Go naming conventions.
4. Optionally, run a linter:
    ```sh
    golint ./...
    ```

## Testing Patterns

- Test files are named with the `*_test.go` suffix.
- Tests are written in Go, but the specific framework is not detected (likely using Go's standard `testing` package).
- Example test file:
    ```go
    // file_system_test.go
    package fs

    import "testing"

    func TestFileSystemInit(t *testing.T) {
        // test logic here
    }
    ```

## Commands
| Command     | Purpose                                      |
|-------------|----------------------------------------------|
| /contribute | Steps for contributing code changes           |
| /test       | Run all tests in the repository               |
| /lint       | Review and check code style before submission |
```
