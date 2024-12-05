#!/bin/bash
# Creating a temporary directory to store the latest updates of google quiche
mkdir -p temp_clone_dir
cd temp_clone_dir

# Cloning the latest updates of google quiche
git clone https://quiche.googlesource.com/quiche google_quiche

# Navigating out of the temporary directory
cd -

# Copying the updates into the quiche working directory i.e. gquiche
cp -fr temp_clone_dir/google_quiche/* gquiche/

# Removing the cloned files
rm -rf temp_clone_dir

# Rewriting changes to the copied files as for the platform implementations
bash utils/google_quiche_rewrite.sh

# Building new client and server with the updates applied
cd build
make simple_quic_server simple_quic_client
cd -