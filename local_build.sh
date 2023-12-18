#!/bin/sh

# exit on errors
set -e

dir=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

sed -i 's|http://archive.ubuntu.com|http://us.archive.ubuntu.com|g' /etc/apt/sources.list
$dir/scripts/install_dependencies.sh

# build out ext components
cd $dir/ext/
./build.sh

cd $dir/container/
cp wait-for-it.sh /usr/local/bin/
cp service.sh /usr/local/bin/

./database.sh
./library.sh
ldconfig

# build internal components
cd $dir/src/
./build.sh release
ldconfig

$dir/scripts/deployment_dependencies.sh

cp $dir/src/tmx/TmxCore/tmxcore.service /lib/systemd/system/
cp $dir/src/tmx/TmxCore/tmxcore.service /usr/sbin/
ldconfig

$dir/container/setup.sh

cd /var/log/tmx/
/usr/local/bin/service.sh
