#
# Copyright (c) 2006 D. Richard Hipp
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the Simplified BSD License (also
# known as the "2-Clause License" or "FreeBSD License".)
#
# This program is distributed in the hope that it will be useful,
# but without any warranty; without even the implied warranty of
# merchantability or fitness for a particular purpose.
#
# Author contact information:
#   drh@hwaci.com
#   http://www.hwaci.com/drh/
#
############################################################################
#
# This is the main test script.  To run a regression test, do this:
#
#     tclsh ../test/tester.tcl ../bld/fossil
#
# Where ../test/tester.tcl is the name of this file and ../bld/fossil
# is the name of the executable to be tested.
#

set testdir [file normalize [file dir $argv0]]
set fossilexe [file normalize [lindex $argv 0]]
set argv [lrange $argv 1 end]

set i [lsearch $argv -halt]
if {$i>=0} {
  set HALT 1
  set argv [lreplace $argv $i $i]
} else {
  set HALT 0
}

set i [lsearch $argv -prot]
if {$i>=0} {
  set PROT 1
  set argv [lreplace $argv $i $i]
} else {
  set PROT 0
}

if {[llength $argv]==0} {
  foreach f [lsort [glob $testdir/*.test]] {
    set base [file root [file tail $f]]
    lappend argv $base
  }
}

# start protocol
#
proc protInit {cmd} {
  if {$::PROT} {
    set out [open "prot" w]
    fconfigure $out -translation platform
    puts $out "starting tests with:$cmd"
    close $out
  }
}

# write protocol
#
proc protOut {msg} {
  puts "$msg"
  if {$::PROT} {
    set out [open "prot" a]
    fconfigure $out -translation platform
    puts $out "$msg"
    close $out
  }
}

# Run the fossil program
#
proc fossil {args} {
  global fossilexe
  set cmd $fossilexe
  foreach a $args {
    lappend cmd $a
  }
  protOut $cmd

  flush stdout
  set rc [catch {eval exec $cmd} result]
  global RESULT CODE
  set CODE $rc
  if {$rc} {puts "ERROR: $result"}
  set RESULT $result
}

# Read a file into memory. 
#
proc read_file {filename} {
  set in [open $filename r]
  fconfigure $in -translation binary
  set txt [read $in [file size $filename]]
  close $in
  return $txt
}

# Write a file to disk
#
proc write_file {filename txt} {
  set out [open $filename w]
  fconfigure $out -translation binary
  puts -nonewline $out $txt
  close $out
}
proc write_file_indented {filename txt} {
  write_file $filename [string trim [string map [list "\n  " \n] $txt]]\n
}

# Return true if two files are the same
#
proc same_file {a b} {
  set x [read_file $a]
  regsub -all { +\n} $x \n x
  set y [read_file $b]
  regsub -all { +\n} $y \n y
  return [expr {$x==$y}]
}

# Perform a test
#
set test_count 0
proc test {name expr} {
  global bad_test test_count
  incr test_count
  set r [uplevel 1 [list expr $expr]]
  if {$r} {
    protOut "test $name OK"
  } else {
    protOut "test $name FAILED!"
    lappend bad_test $name
    if {$::HALT} exit
  }
}
set bad_test {}

# Return a random string N characters long.
#
set vocabulary 01234567890abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
append vocabulary "       ()*^!.eeeeeeeeaaaaattiioo   "
set nvocabulary [string length $vocabulary]
proc rand_str {N} {
  global vocabulary nvocabulary
  set out {}
  while {$N>0} {
    incr N -1
    set i [expr {int(rand()*$nvocabulary)}]
    append out [string index $vocabulary $i]
  }
  return $out
}

# Make random changes to a file.
#
# The file is divided into blocks of $blocksize lines each.  The first
# block is number 0.  Changes are only made within blocks where
# the block number divided by $count has a remainder of $index.
#
# For any given line that mets the block count criteria, the probably
# of a change is $prob
#
# Changes do not add or remove newlines
#
proc random_changes {body blocksize count index prob} {
  set out {}
  set blockno 0
  set lineno -1
  foreach line [split $body \n] {
    incr lineno
    if {$lineno==$blocksize} {
      incr blockno
      set lineno 0
    }
    if {$blockno%$count==$index && rand()<$prob} {
      set n [string length $line]
      if {$n>5 && rand()<0.5} {
        # delete part of the line
        set n [expr {int(rand()*$n)}]
        set i [expr {int(rand()*$n)}]
        set k [expr {$i+$n}]
        set line [string range $line 0 $i][string range $line $k end]
      } else {
        # insert something into the line
        set stuff [rand_str [expr {int(rand()*($n-5))-1}]]
        set i [expr {int(rand()*$n)}]
        set ip1 [expr {$i+1}]
        set line [string range $line 0 $i]$stuff[string range $line $ip1 end]
      }
    }
    append out \n$line
  }
  return [string range $out 1 end]
}

protInit $fossilexe
foreach testfile $argv {
  set dir [file root [file tail $testfile]]
  file delete -force $dir
  file mkdir $dir
  set origwd [pwd]
  cd $dir
  protOut "***** $testfile ******"
  source $testdir/$testfile.test
  protOut "***** End of $testfile: [llength $bad_test] errors so far ******"
  cd $origwd
}
set nErr [llength $bad_test]
protOut "***** Final result: $nErr errors out of $test_count tests"
if {$nErr>0} {
  protOut "***** Failures: $bad_test"
}
