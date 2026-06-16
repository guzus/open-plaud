---
okf_version: "0.1"
title: Open Plaud Knowledge Bundle
description: Agent-readable project knowledge for the open-plaud hardware, firmware, and transcription stack.
---
# Open Plaud Knowledge Bundle

This directory is an Open Knowledge Format bundle for `open-plaud`. It mirrors
the repository's architecture, contracts, and operating workflows as linked
markdown concepts with YAML frontmatter.

# Concepts

* [Open Plaud system](systems/open-plaud.md) - product-level map of the recorder stack.
* [Components](components/) - implementation areas and ownership boundaries.
* [APIs](apis/) - external contracts exposed by the server.
* [Contracts](contracts/) - stable internal data and provider interfaces.
* [Workflows](workflows/) - operational paths humans and agents should follow.

# Source Of Truth

This bundle summarizes repository sources. When there is a conflict, the linked
code and docs in each concept's `# Citations` section are authoritative.
