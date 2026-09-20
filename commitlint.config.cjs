module.exports = {
  extends: ["@commitlint/config-conventional"],
  // Mirrors the repo's observed commit vocabulary (git log census): the
  // Coco->Kirn migration introduces repo-area types (meta, scripts, tools,
  // examples, rename, gitignore, stdlib) alongside the conventional set.
  rules: {
    "type-enum": [
      2,
      "always",
      [
        "build",
        "chore",
        "CI",
        "ci",
        "docs",
        "examples",
        "feat",
        "fix",
        "gitignore",
        "meta",
        "perf",
        "refactor",
        "rename",
        "revert",
        "scripts",
        "stdlib",
        "test",
        "tools"
      ]
    ],
    "header-max-length": [2, "always", 100],
    "body-max-line-length": [2, "always", 100],
    "subject-case": [2, "never", ["sentence-case", "start-case"]]
  }
};
