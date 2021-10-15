Tests in this directory are meant to provide broad coverage of surface-level API
behavior. Tests for each API go in their own source file, and they should
exercise the various error conditions of each API, as well any behavior which
can be validated in isolation without regard for complex side effects across the
system.

Test coverage for more complex interactions is provided by tests in
src/api/integration_tests.
