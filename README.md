[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://github.com/zdenop/Sword.Patched/blob/master/LICENSE)
[![Autotools build](https://github.com/zdenop/Sword.Patched/actions/workflows/autotols.yml/badge.svg)](https://github.com/zdenop/Sword.Patched/actions/workflows/autotols.yml)

# Sword.Patched

This is an unofficial Git fork of [The SWORD Project](https://crosswire.org/sword/), originally developed in their [SVN repository](https://crosswire.org/svn/sword/trunk/).

[Original README](README.org), [Original README for svn](README.svn).

## Purpose

The goal of this fork is to provide community-driven fixes and improvements not yet available in the official repository.

## Branches

- **`master`**: Contains the patched Sword code with fixes and enhancements.
- **`svn_trunk`**: A mirror of the official SVN trunk for reference and syncing.

## Quick Start

1. Clone the repository:

```sh
git clone https://github.com/yourusername/Sword.Patched.git
cd Sword.Patched
```

2. Checkout the desired branch:

```sh
git checkout master  # For patched version
git checkout svn_trunk  # For official SVN mirror
```

3. Follow the official [A quick start for getting the engine compiled](https://crosswire.org/sword/develop/index.jsp) for compilation. 
For more detailes instruction see local [INSTALL.git.md](INSTALL.git.md) file.

## Contributing

Contributions are welcome! Please:
- Fork the repository and create a pull request against the `master` branch.
- Ensure your changes include tests and documentation.
- Reference any related official issues or patches.
