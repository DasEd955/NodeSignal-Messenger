#!/bin/bash

gnome-terminal -- bash -c "./nodesignal_server; exec bash"

for i in {1..3}
do
  gnome-terminal -- bash -c "./nodesignal_client; exec bash"
done
