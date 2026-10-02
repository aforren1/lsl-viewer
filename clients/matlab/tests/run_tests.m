function run_tests(transports)
% RUN_TESTS  Test lslrc.Client against a fake protocol-2 server. Works in MATLAB and Octave.
%
%   run_tests                      every transport that this installation has
%   run_tests({'java'})            only the Java transport
%
% The fake server is tests/fake_server.py. Python 3 must be on the PATH, or set
% LSLRC_PYTHON. If LSL_VIEWER_EXE is set, integration_test also runs.
here = fileparts(mfilename('fullpath'));
addpath(fileparts(here));
if nargin < 1, transports = available_transports(); end
[pid, port] = start_server(here);
cleaner = onCleanup(@() run_spawn({'kill', sprintf('%d', pid)}));
tests = {@t_banner, @t_banner_v1, @t_busy, @t_connect_refused, @t_streams_zero, ...
         @t_streams_many, @t_status_spaces, @t_error_reply, @t_one_segment, ...
         @t_commands, @t_close_on_delete, @t_get_large, @t_get_truncated};
fails = 0;
total = 0;
for ti = 1:numel(transports)
  tr = transports{ti};
  for k = 1:numel(tests)
    name = func2str(tests{k});
    total = total + 1;
    try
      ctl = lslrc.Client('127.0.0.1', port, 'Transport', tr);
      reset_server(ctl);
      tests{k}(port, tr, ctl);
      ctl.close();
      fprintf('PASS  %-10s %s\n', tr, name);
    catch err
      fails = fails + 1;
      fprintf('FAIL  %-10s %s: %s (%s)\n', tr, name, err.message, err.identifier);
    end
  end
end
total = total + 1;
if exist('lsl_loadlib') ~= 0 %#ok<EXIST>
  total = total - 1;
  fprintf('SKIP  discover without liblsl (liblsl-Matlab is on the path)\n');
else
  try
    expect_error(@() lslrc.discover(), 'lslrc:nolsl');
    fprintf('PASS  discover without liblsl\n');
  catch err
    fails = fails + 1;
    fprintf('FAIL  discover without liblsl: %s\n', err.message);
  end
end
total = total + 1;
try
  if integration_test()
    fprintf('PASS  integration_test\n');
  else
    fprintf('SKIP  integration_test (set LSL_VIEWER_EXE to run it)\n');
  end
catch err
  fails = fails + 1;
  fprintf('FAIL  integration_test: %s (%s)\n', err.message, err.identifier);
end
clear cleaner
fprintf('%d of %d tests passed.\n', total - fails, total);
if fails > 0, error('lslrc:test', '%d test(s) failed.', fails); end
end

% ---- Tests. Each gets the server port, the transport, and a control client. ----

function t_banner(port, tr, ctl)
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr);
assert(rc.protocol == 2, 'protocol is %g', rc.protocol);
assert(strcmp(rc.transport, tr));
rc.close();
ctl.command('__next v3');
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr);
assert(rc.protocol == 3, 'a newer protocol must be accepted');
end

function t_banner_v1(port, tr, ctl)
ctl.command('__next v1');
expect_error(@() lslrc.Client('127.0.0.1', port, 'Transport', tr), 'lslrc:protocol');
end

function t_busy(port, tr, ctl)
ctl.command('__next busy');
expect_error(@() lslrc.Client('127.0.0.1', port, 'Transport', tr), 'lslrc:busy');
end

function t_connect_refused(port, tr, ctl) %#ok<INUSL>
% Port 1 (tcpmux) is closed on a normal machine.
expect_error(@() lslrc.Client('127.0.0.1', 1, 'Transport', tr, 'Timeout', 2), 'lslrc:connect');
end

function t_streams_zero(port, tr, ctl)
ctl.command('__streams 0');
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr);
st = rc.streams();
assert(isstruct(st) && isempty(st) && isfield(st, 'key') && isfield(st, 'rate'));
k = rc.selected();
assert(iscell(k) && isempty(k));
% Nothing may be left over to spoil the next reply.
s = rc.status();
assert(~s.recording);
end

function t_streams_many(port, tr, ctl)
ctl.command('__streams 4');
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr);
r = rc.select({'host-a|MockEEG', '{0b4f-uid}'});
assert(strncmp(r, 'ok', 2), r);
st = rc.streams();
assert(numel(st) == 4, 'got %d streams', numel(st));
assert(strcmp(st(1).key, 'host-a|MockEEG') && st(1).channels == 32 && st(1).rate == 500);
assert(st(1).recording && ~st(2).recording && st(3).recording);
assert(st(2).rate == 0 && strcmp(st(2).type, 'Markers') && strcmp(st(2).name, 'Mock Markers'));
assert(strcmp(st(3).name, 'Audio | Left') && strcmp(st(3).key, '{0b4f-uid}'));
assert(st(4).rate == 120 && st(4).channels == 3);
k = rc.selected();
assert(isequal(k, {'host-a|MockEEG'; '{0b4f-uid}'}));
h = rc.command('help');
assert(numel(strfind(h, char(10))) == 2, 'help must return its counted lines');
s = rc.status();
assert(s.streams == 2);
end

function t_status_spaces(port, tr, ctl)
p = 'C:/my data/sub 01/a b=c.xdf';
ctl.command(['__file ' p]);
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr);
s = rc.status();
assert(strcmp(s.file, p), 'file is "%s"', s.file);
assert(islogical(s.recording) && ~s.recording && s.seconds == 0 && s.bytes == 123456789012);
ctl.command('__file ');
s = rc.status();
assert(isempty(s.file) && ischar(s.file));
rc.select('all');
rc.start();
s = rc.status();
assert(s.recording && s.seconds == 12.5 && s.streams == 3);
end

function t_error_reply(port, tr, ctl) %#ok<INUSD>
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr);
expect_error(@() rc.select({'no-such-key'}), 'lslrc:remote');
expect_error(@() rc.select({'a,b'}), 'lslrc:arg');
expect_error(@() rc.select({' padded'}), 'lslrc:arg');
r = rc.command('bogus');
assert(strncmp(r, 'error:', 6), r);
expect_error(@() rc.set('nofield', 'x'), 'lslrc:remote');
expect_error(@() rc.set('subject', sprintf('a\nb')), 'lslrc:arg');
s = rc.status();  % the connection is still in step
assert(~s.recording);
end

function t_one_segment(port, tr, ctl)
% The fake server sends the get header and the data with one send call.
ctl.command('__getsize 100');
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr);
f = [tempname() '.xdf'];
p = rc.get(f);
assert(strcmp(p, f));
check_file(f, 100);
delete(f);
s = rc.status();
assert(~s.recording);
end

function t_commands(port, tr, ctl) %#ok<INUSD>
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr);
assert(strcmp(rc.set('subject', '01'), 'ok: subject = 01'));
assert(strcmp(rc.set('run', 2), 'ok: run = 2'));
assert(strncmp(rc.filename('C:/out dir/x.xdf'), 'ok', 2));
expect_error(@() rc.start(), 'lslrc:remote');  % nothing selected
rc.select('all');
assert(numel(rc.selected()) == 3);
assert(strcmp(rc.start('D:/a b/rec.xdf'), 'ok: recording -> D:/a b/rec.xdf'));
expect_error(@() rc.get(tempdir), 'lslrc:remote');  % still recording
assert(strcmp(rc.stop(), 'ok: stopped'));
rc.select('none');
assert(isempty(rc.selected()));
rc.select('all');
rc.select({});  % an empty list means none
assert(isempty(rc.selected()));
assert(strcmp(rc.command('quit'), 'bye'));
expect_error(@() rc.status(), 'lslrc:closed');
end

function t_close_on_delete(port, tr, ctl)
n0 = clients(ctl);
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr);
assert(clients(ctl) == n0 + 1);
rc.close();
wait_clients(ctl, n0);
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr);
assert(clients(ctl) == n0 + 1);
clear rc  % the destructor must close the socket
wait_clients(ctl, n0);
end

function n = clients(ctl)
n = sscanf(ctl.command('__clients'), 'ok: %d');
end

function wait_clients(ctl, n)
for i = 1:50
  if clients(ctl) == n, return; end
  pause(0.05);
end
error('lslrc:test', 'the server still has %d clients, expected %d', clients(ctl), n);
end

function t_get_large(port, tr, ctl)
n = 20e6;
ctl.command(sprintf('__getsize %d', n));
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr);
d = tempname();
mkdir(d);
tic;
p = rc.get(d);
el = toc;
fprintf('      get of %.0f MB with %s: %.0f MB/s\n', n / 1e6, tr, n / el / 1e6);
assert(strcmp(p, fullfile(d, 'rec file.xdf')), p);
check_file(p, n);
% A second get on the same connection proves the framing stayed in step.
ctl.command('__getsize 5000');
f = fullfile(d, 'second.xdf');
rc.get(f);
check_file(f, 5000);
rmdir(d, 's');
end

function t_get_truncated(port, tr, ctl)
ctl.command('__getsize 3000000');
ctl.command('__truncate');
% MATLAB's tcpclient cannot see a close, only a timeout, thus keep it short.
rc = lslrc.Client('127.0.0.1', port, 'Transport', tr, 'GetTimeout', 2);
d = tempname();
mkdir(d);
try
  rc.get(d);
  error('lslrc:test', 'get did not fail');
catch err
  assert(any(strcmp(err.identifier, {'lslrc:closed', 'lslrc:timeout'})), ...
         'got %s: %s', err.identifier, err.message);
end
left = dir(d);
assert(numel(left) == 2, 'a partial file is left: %s', left(end).name);  % . and ..
rmdir(d, 's');
end

% ---- Helpers ----

function reset_server(ctl)
ctl.command('stop');
ctl.command('select none');
ctl.command('__streams 3');
ctl.command('__getsize 1000');
ctl.command('__getname rec file.xdf');
ctl.command('__file C:/data dir/sub 01/rec file.xdf');
end

function check_file(f, n)
fid = fopen(f, 'rb');
got = fread(fid, Inf, '*uint8');
fclose(fid);
assert(numel(got) == n, 'file has %d bytes, expected %d', numel(got), n);
want = uint8(mod(0:n-1, 251));
assert(isequal(got(:), want(:)), 'file content differs');
end

function expect_error(f, id)
try
  f();
catch err
  assert(strcmp(err.identifier, id), 'expected %s, got %s: %s', id, err.identifier, err.message);
  return;
end
error('lslrc:test', 'expected error %s, got none', id);
end

function t = available_transports()
t = {};
if usejava('jvm'), t{end+1} = 'java'; end
ok = exist('tcpclient') ~= 0; %#ok<EXIST>
if ~ok && exist('OCTAVE_VERSION', 'builtin')
  try
    pkg('load', 'instrument-control');
    ok = true;
  catch %#ok<CTCH>
  end
end
if ok, t{end+1} = 'tcpclient'; end
end

function [pid, port] = start_server(here)
pf = [tempname() '.port'];
out = run_spawn({'start', '--', run_spawn(), fullfile(here, 'fake_server.py'), '--portfile', pf});
pid = str2double(strtrim(out));
for i = 1:100
  if exist(pf, 'file'), break; end
  pause(0.1);
end
fid = fopen(pf, 'r');
if fid < 0, error('lslrc:test', 'The fake server did not start.'); end
port = fscanf(fid, '%d');
fclose(fid);
delete(pf);
end
