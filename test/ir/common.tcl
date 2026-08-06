set testdir [file dirname $argv0]
cd $testdir

set ::base_cmd [list ../../antcc -o/dev/null -c -xc -]
set ::last_cmdargs {}
set ::last_prog ""

proc execv {cmd in} {
    lassign [chan pipe] rderr wrerr
    set pipeline [open |[concat $cmd [list 2>@ $wrerr]] r+]
    puts -nonewline $pipeline $in
    flush $pipeline
    chan close $pipeline write
    close $wrerr
    set stdout [read $pipeline]
    set stderr [read $rderr]
    set rc 0
    if {[catch {close $pipeline} result opts]} {
        if {[dict get $opts -errorcode] eq "CHILDSTATUS"} {
            set rc [lindex [dict get $opts -errorcode] 2]
        } else {
            set rc 1
        }
    }
    list $rc $stdout $stderr
}

proc fail {msg {detail ""}} {
    puts "FAIL: $msg"
    puts "--- prog ---"
    puts $::last_prog
    puts "--------------"
    puts "command: $::base_cmd $::last_cmdargs"
    if {$::last_stdout ne {}} {
        puts "--- stdout ---"
        puts -nonewline $::last_stdout
        puts "--------------"
    }
    if {$::last_stderr ne {}} {
        puts "--- stderr ---"
        puts -nonewline $::last_stderr
        puts "--------------"
    }
    if {$detail ne ""} {
        puts $detail
    }
    exit 1
}

proc c {args prog} {
    set ::last_cmdargs $args
    set ::last_prog $prog
    set res [execv [concat $::base_cmd $args] $prog]
    set ::last_stdout [lindex $res 1]
    set ::last_stderr [lindex $res 2]
    return $res
}

proc c+out {args prog} {
    lassign [c $args $prog] rc stdout
    if {$rc != 0} {
        fail "compile; rc=$rc"
    }
    return $stdout
}

proc asserteq {label actual expected} {
    if {$actual ne $expected} {
        fail "asserteq: $label (expected '$expected', got '$actual')"
    }
}

