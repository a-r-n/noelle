#!/bin/bash

function measureTime {
  local binaryName=$1 ;
  local outputTimeFileName=$2 ;
  local outputFileName=$3 ;

  # Read input for arguments to performance runs
  local ARGS=$(< perf_args.info) ;

  # Create a temporary file
  tempFile=`mktemp` ;
  tempFile2=`mktemp` ;
  tempFile3=`mktemp` ;

  # Measure the execution times
  for j in `seq 0 7` ; do

    # Measure the time
    { time ./$binaryName $ARGS ; } &> $tempFile ;

    # Check if the execution crashed
    if test $? -ne 0 ; then
      echo "ERROR: `pwd` crashed" ;
      echo "0" > $outputTimeFileName ;
      return 1; 
    fi

    # Append the time
    local MATCHER="/real\t(.*)m(.*)s/" ;
    local PRINTER="{ print a[1] * 60 + a[2] }" ;
    local GAWK_CMD=" match(\$0, ${MATCHER}, a) ${PRINTER} " ;
    local timeMeasured=$(gawk "$GAWK_CMD" $tempFile) ;
    echo $timeMeasured >> $tempFile2 ;

  done

  # Dump the output
  if ! test -e $outputFileName ; then
    ./$binaryName $ARGS &> $outputFileName ;
  fi

  # Check if the execution crashed
  if test $? -ne 0 ; then
    echo "ERROR: `pwd` crashed" ;
    echo "0" > $outputTimeFileName ;
    return 1; 
  fi

  # Sort the times
  sort -g $tempFile2 > $tempFile3 ;

  # Fetch the number of lines of the file
  lines=`wc -l $tempFile3 | awk '{print $1}'` ;

   # Compute the median
  median=`echo "$lines / 2" | bc` ;

  # Fetch the median
  result=`awk -v median=$median '
    BEGIN {
      c = 0;
    }{
      if (c == median){
        print ;
      }
      c++;
    }' $tempFile3` ;

  # Print the median
  echo "$result" > $outputTimeFileName ;

  # Clean
  rm $tempFile ;
  rm $tempFile2 ;
  rm $tempFile3 ;

  return ;
}

function runningTests {
  echo $1 ;
  > $4 ;

  # Export autotuner specifications for parallelization
  export INDEX_FILE="autotuner.info" ;

  for i in `ls`; do
    if ! test -d $i ; then
      continue ;
    fi
    echo -e " Testing `basename $i` " ;
    echo -ne "$i\\t" >> $4 ;

    # Go to the test directory
    pushd ./ ;
    cd $i ;

    # Clean
    # echo "   Make clean " ;
    make clean > /dev/null ; 

    # Compile
    # echo "   Make " ;
    make NOELLE_OPTIONS="$2" PARALLELIZATION_OPTIONS="$3" >> compiler_output.txt 2>&1 ;

    # Read input for arguments to performance runs
    local ARGS=$(< perf_args.info) ;

    # Measure the baseline
    if ! test -f time_baseline.txt ; then 
      echo -e "  Running baseline " ;
      measureTime baseline time_baseline.txt baseline_output.txt ;
      if test $? -ne 0 ; then
        popd ;
        echo "0" >> $4 ;
        continue ;
      fi
    fi
    local BASE=`cat time_baseline.txt` ;

    # Measure the parallelized binary
    echo -e "  Running performance " ;
    measureTime parallelized time_parallelized.txt output_parallelized.txt ;
    local PAR=`cat time_parallelized.txt` ;

    # Check if the outputs match
	  cmp baseline_output.txt output_parallelized.txt &> /dev/null ;
    if test $? -ne 0 ; then 
      echo "ERROR: `pwd` did not generate the correct output" ;
      popd ;
      echo "0" >> $4 ;
      continue ;
    fi
    popd ;

    # Dump the speedup
    local SPEEDUP=$(bc <<< " scale=3; $BASE / $PAR ") ;
    echo -e "  Speedup: $SPEEDUP" ;
    echo $SPEEDUP >> $4 ;
  done

  echo "Done"
}

export PATH=`pwd`/../install/bin:$PATH ;

# Run
cd performance ;
runningTests "Measuring the default configuration" "-noelle-verbose=3" " " "speedups.txt" ;

cd ../ ;

exit 0;
