#!/usr/bin/perl -w

use Env;

if ( ! $STP_CLIENT_API ) 
{
	print "No STP environment.  Exiting.\n";
	exit -1;
}

# This will take two optional arguments:
# STEP: Min value and The step to use 
# MAX: The maximum # of groups to reach in testing

if ($#ARGV == 1)
{
  $STEP = $ARGV[0];
  $MAX = $ARGV[1];
} else {
  $STEP = 20;
  $MAX = 100;
}

# We suggest that unless you want to build in package listings for each possible
# target OS, you should have a script ready to automate the build process.

print "\nWelcome to the hackbench test\n\n";
print "---Making the binaries (via make clean all)---\n";
print `rm -rf bin`;
print `tar -zxf hackbench_bin.tar.gz`;
chdir "bin";
print `make clean all 1> /dev/null`;
print "---Done---\n\n"; 

# Create final index.html
open FHI, ">$WRAP_HOME/index.html";
open FHO, "<$RESULTS_HEADER";
while(<FHO>){
	print FHI;
}
close FHO;
open FHO, "<$WRAP_HOME/index.body";
while(<FHO>){
	print FHI;
}
close FHO;
open FHO, "<$RESULTS_FOOTER";
while(<FHO>){
	print FHI;
}
close FHI;
close FHO;

open (FILE, ">>$RESULTS/data") || die "cannot append to results file";
print FILE "# Hackbench Results\n";
print FILE "# -------------------------------\n";
print FILE "# Processes | Ave Time(sec)\n";
print FILE "# -------------------------------\n";

# REMEMBER, any output form this file is sent as an email to the user, make sure it's
# short, informative and not filled with binary error spews.

print "========================\n";
print "Executing hackbench runs\n";
print "========================\n\n";

# This is a basic run with simple values

# The system makes this directory now
#system "rm -rf $RESULTS";
#mkdir "$RESULTS";

@times = ();

print "Running with $STEP to $MAX groups, with step $STEP\n";

system ("/usr/bin/stp_timeout.sh 2h hackbench &");

system ("stp_profile 'Test set-up complete.'");

for ($numgr = $STEP; $numgr <= $MAX; $numgr += $STEP )
{
  print "Running with $numgr groups";

  for ($run = 0; $run < 5; $run++)
  {

    if ( `./hackbench $numgr` =~ m/Time: ([0-9.]+)/ ) {
      $time = $1;
    }
    push(@times, $time);
    print ".";
  }

  $ave = 0;
  foreach $time (@times)
  {
    $ave += $time;
  }
  $ave = $ave / scalar(@times);
  print " Average $ave s\n";
  
  print FILE  "$numgr 		$ave\n";
}
close FILE;

system ("stp_profile 'Run test.'");

open FHO,  "<$RESULTS/data"; 
open FHI,  ">>$RESULTS_EMAIL";
while (<FHO>){
	print  FHI;
}
close FHO; 
close FHI; 

# Make the plot
system "cp -a $WRAP_HOME/graph.cfg $RESULTS/";
chdir $RESULTS;
chmod 0755, 'graph.cfg';
system ("$RESULTS/graph.cfg");

exit;
