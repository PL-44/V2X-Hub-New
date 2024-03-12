#! /bin/bash

set -e 

V2XHOME="$HOME/V2X-Hub"
V2X_SRC="$V2XHOME/src"
V2X_CONFIG="$V2XHOME/configuration/amd64"

#Copy source to container
#cd "$V2X_SRC" && sudo docker cp . v2xhub:/home/V2X-Hub/src/

#Remove already running container
#cd "$V2X_CONFIG" && sudo docker compose down v2xhub

#Run build
cd "$V2XHOME" && sudo docker build -t usdotfhwaops/v2xhub:latest .

#Start up the container
cd "$V2X_CONFIG" && sudo docker compose up -d
