Contributing to Seastar
=======================

# Sending Patches

Seastar follows a patch-submission workflow similar to that of the Linux kernel. Send patches to `seastar-dev` with a DCO sign-off. Use `git send-email` to send your patch.

Example:

1. When you commit, use `-s` in your Git command. This adds a sign-off for the [Developer Certificate of Origin](https://developercertificate.org/).

   You can prefix the commit message with a tag identifying the area of the codebase that the patch addresses:

        git commit -s -m "core: some descriptive commit message"

2. Then send an email to the Google Group:

        git send-email <revision>..<final_revision> --to seastar-dev@googlegroups.com

When replying to patches, use `--in-reply-to` with the message ID of the original message. If you are sending a new version of a change, use `git rebase` and then `git send-email` with an option such as `-v2` to indicate that it is the second version.

# Testing and Approval

Run `test.py` and ensure that tests pass at least as well as they did before the patch.
