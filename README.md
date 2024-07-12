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

[Install docker](https://docs.docker.com/engine/install/ubuntu/#set-up-the-repository) to run binary inside cgroup

## Running

Setup you shell session with:
```bash
cd stress_memcg
source env.sh.inc
```

Then use `run-test` and `stop-test` to run and stop the stress test.
