#!/bin/bash
# Author: Bo Li (li-bo@outlook.com)
#
# This is a shell script, but it's recommended that you run the commands one by
# one by copying and pasting into the shell.
#
# This script contains some experimental setups, use with cautions.
#

set -e           #Exit on non-zero return code from any command
set -o pipefail  #Exit if any of the commands in the pipeline will 
                 #return non-zero return code
set -u           #Fail on an undefined variable

. ./cmd.sh ## You'll want to change cmd.sh to something that will work on your system.
           ## This relates to the queue.
. ./path.sh ## update system path

log_start(){
  echo "#####################################################################"
  echo "Spawning *** $1 *** on" `date` `hostname`
  echo ---------------------------------------------------------------------
}

log_end(){
  echo ---------------------------------------------------------------------
  echo "Done *** $1 *** on" `date` `hostname` 
  echo "#####################################################################"
}

####################################
# experiment of RBM(uttbias) - "rub"
rub_pretrain(){
  #RBM pretrain
  log_start "rub1a [pretrain]"
  init=exp_multi/tri2a_dnn_pretrain/1.rbm
  dir=exp_multi/rub1a_dnn_pretrain
  mkdir -p $dir/log
  steps/aurora4/rbmdnn/rbmuttbias_pretrain.sh --init-uttbias-rbm $init --uttbias-rbm-iter 20 --nn-depth 7 --rbm-iter 3 --norm-vars true --splice 5 feat/fbank/train_multi $dir
  log_end "rub1a [pretrain]"
}
rub_pretrain


