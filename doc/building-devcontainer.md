## Building seastar using devcontainers

A development container (or devcontainer for short) allows you to use a container as a full-featured development environment. It can be used to run an application, to separate tools, libraries, or runtimes needed for working with a codebase, and to aid in continuous integration and testing.

You can build a project with [devcontainers](https://containers.dev/) in an easy and convenient way.
Your IDE (e.g. Clion) or code editor (e.g. VS Code) can run and [attach](https://containers.dev/supporting) to devcontainer.

Also you can use [devcontainers/cli](https://github.com/devcontainers/cli) to set up environment and build the project manually via bash:

```bash
devcontainer up --workspace-folder .

devcontainer exec --workspace-folder . \
  ./configure.py --mode=release

devcontainer exec --workspace-folder . \
  ninja -C build/release
```

It's just a more powerful and convenient version of [building-docker](./building-docker.md)
