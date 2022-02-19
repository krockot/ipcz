This directory is a bag of utilities. Everything here is generally a leaf-node
dependency with two key features:

- typically but not necessarily used by more than one component (ipcz
  implementation, tests, drivers) within the repository.
- no interesting dependencies on any part of the tree except other util/
  definitions or the main ipcz.h header. Abseil dependencies are OK.

