## Building on Ubuntu
Install Prerequisites:
```bash
sudo apt-get update
sudo apt-get install git cmake
```

Install [clang++](https://apt.llvm.org/) compiler:
```bash
sudo bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)"
```

Clone repository:
```
git clone https://github.com/serxa/stress_memcg.git
```

Build the binary:
```bash
cd stress_memcg
./build.sh
```

## Prebuilt binaries
You can find binaries for x86-64 and aarch64 in [Releases](https://github.com/serxa/stress_memcg/releases)
```
cd stress_memcg
mkdir -p build
wget https://github.com/serxa/stress_memcg/releases/download/v1.0.0/stress_memcg_x86-64 -O build/stress_memcg
... or ...
wget https://github.com/serxa/stress_memcg/releases/download/v1.0.0/stress_memcg_aarch64 -O build/stress_memcg
chmod +x build/stress_memcg
```

## Running
[Install docker](https://docs.docker.com/engine/install/ubuntu/#set-up-the-repository) to run binary inside cgroup

Setup you shell session with:
```bash
cd stress_memcg
source env.sh.inc
```

Then use `run-test` and `stop-test` to run and stop the stress test.

## Running on GKE node
First, you'll need to obtain the node shell.

Run and exit `toolbox` to create a folder without `noexec` mount flag. It should output a path for you, use it to construct the path to enter by adding `/var/lib` for example:
```
# toolbox
Spawning container root-us.gcr.iocos-cloudtoolbox-v12345678 on /var/lib/toolbox/root-us.gcr.io_cos-cloud_toolbox-v12345678.
Press Ctrl-] three times within 1s to kill container.
(Hit Ctrl-D)
cd /var/lib/toolbox/root-us.gcr.io_cos-cloud_toolbox-v12345678/var/lib
```

In the folder download the repo and the binary:
```bash
git clone https://github.com/serxa/stress_memcg.git
cd stress_memcg
mkdir -p build
wget https://github.com/serxa/stress_memcg/releases/download/v1.0.0/stress_memcg_x86-64 -O build/stress_memcg
chmod +x build/stress_memcg
```

Setup shell and run the test
```bash
source env.sh.inc
run-test
```

Optional. On another node shell run checker:
```
cd /var/lib/toolbox/root-us.gcr.io_cos-cloud_toolbox-v12345678/var/lib/stress_memcg
source env.sh.inc
checker `pgrep stress_memcg`
```
