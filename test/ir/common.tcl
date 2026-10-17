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

set ::arithops {
    neg not cvtf32s cvtf32u cvtf32f64 cvtf64s cvtf64u cvtf64f32
    cvts32f cvtu32f cvts64f cvtu64f
    exts8 extu8 exts16 extu16 exts32 extu32 bswap16 bswap32 bswap64
    ctz clz popcnt clrsb
    add sub mul div udiv rem urem and ior xor shl sar slr
    equ neq lth gth lte gte ulth ugth ulte ugte
    copy
}

proc findarithins {f {prog_ ""}} {
    if {$prog_ eq ""} {
        set prog_ $::prog
    }
    global cflags arithops
    set out [c+out [concat "-dfilter=$f" $cflags] $prog_]
    set res {}
    foreach line [split $out \n] {
        if {![regexp {= (\m\w+\M)} $line -> op]} continue
        if {[lsearch -exact $arithops $op] < 0} continue
        lappend res $op
    }
    return $res
}

proc findcalls {f {prog_ ""}} {
    if {$prog_ eq ""} {
        set prog_ $::prog
    }
    global cflags
    set matches [regexp -inline -all {= call \$(\w+)} [c+out [concat "-dfilter=$f" $cflags] $prog_]]
    return [lmap {x0 x1} $matches {set x1}]
}

proc findarithwithrhs {f {prog_ ""}} {
    if {$prog_ eq ""} {
        set prog_ $::prog
    }
    global cflags arithops
    set out [c+out [concat "-dfilter=$f" $cflags] $prog_]
    set matches [regexp -inline -all {= (\m\w+\M)[^,]*,\s*(\S+)} $out]
    set res {}
    foreach {full ins rhs} $matches {
        if {[lsearch -exact $arithops $ins] < 0} continue
        lappend res [list $ins $rhs]
    }
    return $res
}

proc findret {f {prog_ ""}} {
    if {$prog_ eq ""} {
        set prog_ $::prog
    }
    global cflags
    return [regexp -inline -line -all {ret .*$} [c+out [concat "-dfilter=$f" $cflags] $prog_]]
}
