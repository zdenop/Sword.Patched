
# Instruction for building library from source

```sh
git clone https://github.com/zdposter/Sword.Patched.git
cd Sword.Patched
```


## Linux / Ubuntu 24.4 LTS build

### Install dependacies

```sh
sudo apt update
sudo apt-get install pkg-config autoconf automake libtool subversion
sudo apt-get install curl libicu-dev libcurl4-openssl-dev
sudo apt-get install liblzma-dev lzma xz-utils git-svn
sudo apt-get install libxapian-dev xapian-examples python3-xapian libxapian30 libclucene-dev libicu-dev libbz2-dev
```

### Build with autotools

```sh
./autogen.sh
./configure --prefix=/usr --with-cxx11regex --with-cxx11time --with-bzip2 --with-xz
make
sudo make install
sudo ldconfig
sudo make install_config
```

### Test of installation

```sh
sudo installmgr --allow-internet-access-and-risk-tracing-and-jail-or-martyrdom -sc -r CrossWire -ri CrossWire KJV
/usr/bin/diatheke -b KJV -f plain -k Jn 11:35
```
