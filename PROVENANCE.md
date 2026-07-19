# Provenance

pgColumnar is built with a clean-room method so that it is free of any copyright
tie to other columnar projects and can be released under the MIT License.

## Roles

- Specification role. A context that read prior columnar source extracted only
  functional and interoperability facts into
  design/FORMAT_AND_INTERFACE_SPEC.md. That document contains no source and no
  implementation expression. It records the on-disk format, the metadata
  catalog, identifier encodings, compression codes, and the SQL surface.
- Implementation role. A separate context writes all code, build files, and
  tests using only the specification, the delivery plan, and the public
  PostgreSQL documentation and headers.

## Rules for implementers

- Do not read, copy, or reference the source of any other columnar project.
- Do not open the prior AGPL source tree. It is kept in a separate location and
  is never checked out beside this repository.
- Build only from design/FORMAT_AND_INTERFACE_SPEC.md, design/REWRITE_PLAN.md,
  and the public PostgreSQL API.
- Correctness may be checked by running the prior extension and comparing
  observable behavior. Running a program is not copying it. Do not copy its test
  files or expected output.

## Log

- 2026-07-18. Specification role produced design/FORMAT_AND_INTERFACE_SPEC.md and
  design/REWRITE_PLAN.md.
- 2026-07-18. Repository created, MIT License applied, specification and plan
  imported. Implementation role assigned to a fresh context working only from the
  specification.
