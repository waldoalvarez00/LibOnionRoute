#!/usr/bin/perl -w

if ($ARGV[0] =~ /^-/) {
    $lang = shift @ARGV;
    $C = ($lang eq '-C');
#    $TXT = ($lang eq '-txt');
}

for $fn (@ARGV) {
    open(F, "$fn");
    $lastnil = 0;
    $lastline = "";
    $incomment = 0;
    while (<F>) {
        ## Warn about windows-style newlines.
        if (/\r/) {
            print "       CR:$fn:$.\n";
        }
        ## Warn about tabs.
        if (/\t/) {
            print "      TAB:$fn:$.\n";
        }
        ## Warn about markers that don't have a space in front of them
        if (/^[a-zA-Z_][a-zA-Z_0-9]*:/) {
            print "nosplabel:$fn:$.\n";
        }
        ## Warn about trailing whitespace.
        if (/ +$/) {
            print "Space\@EOL:$fn:$.\n";
        }
        ## Warn about control keywords without following space.
        if ($C && /\s(?:if|while|for|switch)\(/) {
            print "      KW(:$fn:$.\n";
        }
        ## Warn about #else #if instead of #elif.
        if (($lastline =~ /^\# *else/) and ($_ =~ /^\# *if/)) {
            print " #else#if:$fn:$.\n";
        }
        ## Warn about some K&R violations
        if (/^\s+\{/ and $lastline =~ /^\s*(if|while|for|else if)/ and
	    $lastline !~ /\{$/) {
            print "non-K&R {:$fn:$.\n";
	}
        if (/^\s*else/ and $lastline =~ /\}$/) {
	    print "  }\\nelse:$fn:$.\n";
	}
        $lastline = $_;
        ## Warn about unnecessary empty lines.
        if ($lastnil && /^\s*}\n/) {
            print "  UnnecNL:$fn:$.\n";
        }
        ## Warn about multiple empty lines.
        if ($lastnil && /^$/) {
            print " DoubleNL:$fn:$.\n";
        } elsif (/^$/) {
            $lastnil = 1;
        } else {
            $lastnil = 0;
        }
        ## Terminals are still 80 columns wide in my world.  I refuse to
        ## accept double-line lines.
        if (/^.{80}/) {
            print "     Wide:$fn:$.\n";
        }
        ### Juju to skip over comments and strings, since the tests
        ### we're about to do are okay there.
        if ($C) {
            if ($incomment) {
                if (m!\*/!) {
                    s!.*?\*/!!;
                    $incomment = 0;
                } else {
                    next;
                }
            }
            if (m!/\*.*?\*/!) {
                s!\s*/\*.*?\*/!!;
            } elsif (m!/\*!) {
                s!\s*/\*!!;
                $incomment = 1;
                next;
            }
            s!"(?:[^\"]+|\\.)*"!"X"!g;
            next if /^\#/;
            ## Warn about C++-style comments.
            if (m!//!) {
                #    print "       //:$fn:$.\n";
                s!//.*!!;
            }
            ## Warn about unquoted braces preceded by non-space.
            if (/([^\s'])\{/) {
                print "       $1\{:$fn:$.\n";
            }
            ## Warn about multiple internal spaces.
            #if (/[^\s,:]\s{2,}[^\s\\=]/) {
            #    print "     X  X:$fn:$.\n";
            #}
            ## Warn about { with stuff after.
            #s/\s+$//;
            #if (/\{[^\}\\]+$/) {
            #    print "     {X:$fn:$.\n";
            #}
            ## Warn about function calls with space before parens.
            if (/(\w+)\s\(([A-Z]*)/) {
                if ($1 ne "if" and $1 ne "while" and $1 ne "for" and
                    $1 ne "switch" and $1 ne "return" and $1 ne "int" and
                    $1 ne "elsif" and $1 ne "WINAPI" and $2 ne "WINAPI" and
                    $1 ne "void" and $1 ne "__attribute__" and $1 ne "op") {
                    print "     fn ():$fn:$.\n";
                }
            }
            ## Warn about functions not declared at start of line.
            if ($in_func_head ||
                ($fn !~ /\.h$/ && /^[a-zA-Z0-9_]/ &&
                 ! /^(?:const |static )*(?:typedef|struct|union)[^\(]*$/ &&
                 ! /= *\{$/ && ! /;$/)) {
                if (/.\{$/){
                    print "fn() {:$fn:$.\n";
                    $in_func_head = 0;
                } elsif (/^\S[^\(]* +\**[a-zA-Z0-9_]+\(/) {
                    $in_func_head = -1; # started with tp fn
                } elsif (/;$/) {
                    $in_func_head = 0;
                } elsif (/\{/) {
                    if ($in_func_head == -1) {
                        print "tp fn():$fn:$.\n";
                    }
                    $in_func_head = 0;
                }
            }
        }
    }
    if (! $lastnil) {
        print "  EOL\@EOF:$fn:$.\n";
    }
    close(F);
}

