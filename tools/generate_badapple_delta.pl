#!/usr/bin/env perl
use strict;
use warnings;

my ($input, $output) = @ARGV;
die "usage: $0 badapple.txt assets/badapple.delta\n" unless defined $input && defined $output;

open my $in, '<:raw', $input or die "open $input: $!";
local $/;
my $data = <$in>;
close $in;

my @frames = grep { /\S/ } split /nekomark/, $data;
my ($rows, $cols) = (60, 151);
my $pixels = $rows * $cols;
my @previous = (0) x $pixels;

open my $out, '>:raw', $output or die "open $output: $!";
print {$out} "TBA2";
print {$out} pack('v v v', $rows, $cols, scalar @frames);

for my $frame (@frames) {
    $frame =~ s/^\R+|\R+$//g;
    my @lines = split /\R/, $frame;
    my @current;

    for my $row (0 .. $rows - 1) {
        my $line = $lines[$row] // '';
        $line .= '.' x $cols;
        $line = substr($line, 0, $cols);
        push @current, map { ($_ eq '.' || $_ eq ' ') ? 0 : 1 } split //, $line;
    }

    my @segments;
    my $pos = 0;
    my $last = 0;
    while ($pos < $pixels) {
        while ($pos < $pixels && $current[$pos] == $previous[$pos]) {
            $pos++;
        }
        last if $pos >= $pixels;

        my $start = $pos;
        while ($pos < $pixels && $current[$pos] != $previous[$pos]) {
            $pos++;
        }
        push @segments, [$start - $last, $pos - $start];
        $last = $pos;
    }

    die "too many segments in one frame\n" if @segments > 65535;
    print {$out} pack('v', scalar @segments);
    for my $segment (@segments) {
        die "segment too large\n" if $segment->[0] > 65535 || $segment->[1] > 65535;
        print {$out} pack('v v', $segment->[0], $segment->[1]);
    }
    @previous = @current;
}
close $out;
