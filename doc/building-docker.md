## Building seastar in Docker container

To build a Docker image:

```
docker build -t seastar-dev -f docker/dev/Dockerfile .
```

Building is done with two commands:

```
$ ./configure.py
$ ninja -C build/release
```

You can run them inside container, e.g. like this

```
$ seabuild() { docker run -v $HOME/seastar/:/seastar -u $(id -u):$(id -g) -w /seastar -t seastar-dev "$@"; }
$ seabuild ./configure.py
$ seabuild ninja -C build/release
```
