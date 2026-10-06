SynthOS quick tour
==================

Moving around
  ls, cd, pwd, tree, find      browse files      (try: tree /)
  cat, more, edit FILE         read and edit     (edit has syntax colors)
  cp, mv, rm, mkdir, touch     manage files
  Your /home folder is saved on disk when SynthOS runs from its boot disk.

Running programs
  hello, fib 20, primes 50000, guess, snake    native SX programs in /bin
  hello.com, files.com, box.com, mzhello.exe   MS-DOS programs in /dos
  winhello, wincpp, wincolor, windll           Windows programs in /apps
  asm /home/src/hello.sasm                     build your own SX program
  formats                                      list supported formats

Making it yours
  theme                    list themes (theme dracula, theme matrix ...)
  prompt                   choose a prompt style (prompt twoline)
  config                   all settings; 'config save' keeps them
  color accent pink        recolor any part of the interface
  config statusbar on      status bar with clock and memory

Shell power
  ls /bin | grep sx > /home/list.txt    pipes and redirection
  for f in /home/src/*.sasm; echo $f; end
  if test -f /home/list.txt; echo yes; else echo no; end
  func greet; echo "Hello, $1!"; end; greet World
  let x = 6 * 7; echo $x                arithmetic
  Tab completes, Up/Down walk history, Right accepts a suggestion.

Documentation lives in /docs - try: more /docs/shell.txt or more /docs/windows.txt
