## Building seastar in Docker container

To build a Docker image:

```
docker build -t seastar-dev docker/dev
```

Create an shell function for building insider the container (bash syntax given):

```
$ seabuild() { docker run -v $HOME/seastar/:/seastar -u $(id -u):$(id -g) -w /seastar -t seastar-dev "$@"; }
```

(it is recommended to put this inside your .bashrc or similar)

To build inside a container:

```    
$ seabuild ./configure.py
$ seabuild ninja-build
```

