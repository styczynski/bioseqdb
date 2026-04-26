#! /usr/bin/env bash


BIT=28

echo "[Info] build_rmis_dna script for BWA-MEME 1.0.6v"
if  ! command -v bwa-meme-train-prmi &> /dev/null 
then
    # bwa-meme-train-prmi command not found, should build it
    # To build the command, RMI folder should exist
    if [ ! -d RMI ] 
    then
        # echo "ERROR:"
        echo ERROR: BWA-MEME/RMI directory does not exist or run this script in BWA-MEME folder
        exit 1
    fi

    # check Cargo exists
    if ! command -v cargo &> /dev/null
    then
        echo "[Error] Command: cargo could not be found, please install cargo from https://rustup.rs/"
        exit 1
    fi
    # build the binary 
    cd RMI && RUSTFLAGS=-Awarnings cargo build --release && cd ..
else
    VERSION=`bwa-meme-train-prmi --version`
    if [ ! "$VERSION" = "RMI Learner 0.2" ]
    then
        echo "[Info] Trying to build locally!"
        if [ ! -d RMI ] 
        then
            # echo "ERROR:"
            echo ERROR: BWA-MEME/RMI directory does not exist or run this script in BWA-MEME folder
            exit 1
        fi
        # check Cargo exists
        if ! command -v cargo &> /dev/null
        then
            echo "[Error] Command: cargo could not be found, please install cargo from https://rustup.rs/"
            exit 1
        fi
        # build the binary 
        cd RMI && RUSTFLAGS=-Awarnings cargo build --release && cd ..
    fi
fi

if [ -z "$1" ]; then
    echo "Usage: build_rmis_dna.sh <reference file>
    ex 1) ./build_rmis_dna.sh ./human.fasta        # default setting
    ex 2) ./build_rmis_dna.sh ./human.fasta 26     # set number of models to use for second layer 2^26" 
    echo "About: Learned-index training script for BWA-MEME. For human reference, training requires around 15 minutes and 64GB memory. You can set number of models to use for Learned-index (as in ex 2)."
    exit 0
fi

if [ ! -f $1.suffixarray_uint64 ] 
then
    # echo "ERROR:"
    echo "ERROR: suffixarray_uint64 file does not exists in `dirname $1`
Correct the path or build the suffixarray index first with command: bwa-meme index"
    exit 1
fi

if ! command -v stat -L &> /dev/null
then
    echo "[Info] stat command could not be found, using default setting to build Learned-index"
else
    FILESIZE=$(stat -c%s -L "$1.suffixarray_uint64")
    if [ "$FILESIZE" -gt "8000000000" ]
    then
        BIT=28
    elif [ "$FILESIZE" -gt "1000000000" ]
    then
        BIT=26
    else 
        BIT=24
    fi

fi

if  [ $# -eq 2 ]
  then
  if  [[ $2 == ?(-)+([[:digit:]]) ]]
  then
    array=(20 22 24 26 28 30)
    FOUND=0
    for i in "${array[@]}"
        do
            if [ "$i" -eq "$2" ] ; then
                FOUND=1
            fi
        done
    if [ $FOUND -eq 1 ]
    then
      echo "Set number of model to $2"
    else
      echo "[ERROR] You should select Bit among (20, 22, 24, 26, 28, 30)"
      exit 1
    fi
    
    BIT=$2
  else
    echo "If you intended to set number of models, please give Integer for Bit, current input: $2"
    exit 1
  fi
    
fi

echo "Generate 2^$BIT leaf-models: $(( 1 << $BIT )) models"

function build_rmi_set() {
    DATA_PATH=$1
    DATA_NAME=`basename $1`
    DIR_NAME=`dirname $1`
    
    shift 1
    if  command -v RMI/target/release/bwa-meme-train-prmi &> /dev/null 
    then
	    RMI/target/release/bwa-meme-train-prmi --data-path $DIR_NAME $DATA_PATH $DATA_NAME pwl$BIT,linear,linear_spline $(( 1 << $BIT ))
    elif command -v bwa-meme-train-prmi &> /dev/null 
    then
	    bwa-meme-train-prmi --data-path $DIR_NAME $DATA_PATH $DATA_NAME pwl$BIT,linear,linear_spline $(( 1 << $BIT ))
    else
        echo ERROR: BWA-MEME/RMI directory does not exist or run this script in BWA-MEME folder
    fi
}

build_rmi_set $1.suffixarray_uint64
