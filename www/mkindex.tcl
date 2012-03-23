#!/bin/sh
#
# Run this TCL script to generate a WIKI page that contains a 
# permuted index of the various documentation files.
#
#    tclsh mkindex.tcl >permutedindex.wiki
#

set doclist {
  bugtheory.wiki {Bug Tracking In Fossil}
  branching.wiki {Branching, Forking, Merging, and Tagging}
  build.wiki {Building and Installing Fossil}
  checkin_names.wiki {Checkin And Version Names}
  changes.wiki {Fossil Changelog}
  copyright-release.html {Contributor License Agreement}
  concepts.wiki {Fossil Core Concepts}
  contribute.wiki {Contributing Code or Documentation To The Fossil Project}
  custom_ticket.wiki {Customizing The Ticket System}
  delta_encoder_algorithm.wiki {Fossil Delta Encoding Algorithm}
  delta_format.wiki {Fossil Delta Format}
  embeddeddoc.wiki {Embedded Project Documentation}
  event.wiki {Events}
  faq.wiki {Frequently Asked Questions}
  fileformat.wiki {Fossil File Format}
  foss-cklist.wiki {Checklist For Successful Open-Source Projects}
  fossil-v-git.wiki {Fossil Versus Git}
  index.wiki {Home Page}
  inout.wiki {Import And Export To And From Git}
  makefile.wiki {The Fossil Build Process}
  password.wiki {Password Management And Authentication}
  pop.wiki {Principles Of Operations}
  private.wiki {Creating, Syncing, and Deleting Private Branches}
  qandc.wiki {Questions And Criticisms}
  quickstart.wiki {Fossil Quick Start Guide}
  quotes.wiki
      {Quotes: What People Are Saying About Fossil, Git, and DVCSes in General}
  ../test/release-checklist.wiki {Pre-Release Testing Checklist}
  selfcheck.wiki {Fossil Repository Integrity Self Checks}
  selfhost.wiki {Fossil Self Hosting Repositories}
  server.wiki {How To Configure A Fossil Server}
  settings.wiki {Fossil Settings}
  shunning.wiki {Shunning: Deleting Content From Fossil}
  stats.wiki {Performance Statistics}
  style.wiki {Source Code Style Guidelines}
  ssl.wiki {Using SSL with Fossil}
  sync.wiki {The Fossil Sync Protocol}
  tech_overview.wiki {A Technical Overview Of The Design And Implementation
                      Of Fossil}
  tech_overview.wiki {SQLite Databases Used By Fossil}
  theory1.wiki {Thoughts On The Design Of The Fossil DVCS}
  webui.wiki {The Fossil Web Interface}
  wikitheory.wiki {Wiki In Fossil}
}

set permindex {}
set stopwords {fossil and a in of on the to are about used by for or}
foreach {file title} $doclist {
  set n [llength $title]
  lappend permindex [list $title $file]
  for {set i 0} {$i<$n-1} {incr i} {
    set prefix [lrange $title 0 $i]
    set suffix [lrange $title [expr {$i+1}] end]
    set firstword [string tolower [lindex $suffix 0]]
    if {[lsearch $stopwords $firstword]<0} {
      lappend permindex [list "$suffix &#151; $prefix" $file]
    }
  }
}
set permindex [lsort -dict $permindex]
set out [open permutedindex.wiki w]
puts $out "<title>Permuted Index Of Fossil Documentation</title>"
puts $out "<nowiki>"
puts $out "<ul>"
foreach entry $permindex {
  foreach {title file} $entry break
  puts $out "<li><a href=\"$file\">$title</a></li>"
}
puts $out "</ul>"
