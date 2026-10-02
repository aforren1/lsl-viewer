function ran = integration_test()
% INTEGRATION_TEST  Drive a real lsl-viewer. Runs only if LSL_VIEWER_EXE is set.
%   Starts the viewer with LSL_RC_PORT=22422 LSL_DEMO=1 LSL_PORTABLE=1, records a few seconds of the
%   demo streams, and fetches the file. Kills only the viewer that it started.
exe = getenv('LSL_VIEWER_EXE');
ran = ~isempty(exe);
if ~ran, return; end
here = fileparts(mfilename('fullpath'));
addpath(fileparts(here));
port = 22422;
% Portable mode keeps imgui.ini beside the exe, so the test's recording path does not
% become the default of the user's own viewer.
pid = str2double(strtrim(run_spawn({'start', 'LSL_RC_PORT=22422', 'LSL_DEMO=1', 'LSL_PORTABLE=1', '--', exe})));
cleaner = onCleanup(@() run_spawn({'kill', sprintf('%d', pid)}));
rc = [];
for i = 1:60
  try
    rc = lslrc.Client('127.0.0.1', port);
    break;
  catch err
    if ~strcmp(err.identifier, 'lslrc:connect'), rethrow(err); end
    pause(0.5);
  end
end
if isempty(rc), error('lslrc:test', 'The viewer did not open port %d.', port); end
assert(rc.protocol >= 2);
% Other viewers with LSL_DEMO=1 add their own streams, thus pick keys from the list.
st = [];
for i = 1:40
  st = rc.streams();
  if ~isempty(st), break; end
  pause(0.5);
end
assert(~isempty(st), 'no streams found');
keys = {st(1:min(2, numel(st))).key};
rc.select(keys);
assert(isempty(setdiff(keys, rc.selected())));
rc.set('subject', '01');
rc.set('task', 'lslrc');
d = tempname();
mkdir(d);
mkdir(fullfile(d, 'rec'));
mkdir(fullfile(d, 'got'));
rc.start(fullfile(d, 'rec', 'lslrc_it.xdf'));
pause(2);
s = rc.status();
assert(s.recording && s.streams >= 1, 'not recording');
rc.stop();
s = rc.status();
assert(~s.recording);
p = rc.get(fullfile(d, 'got'));
fid = fopen(p, 'rb');
magic = fread(fid, 4, '*char').';
fclose(fid);
assert(strcmp(magic, 'XDF:'), 'the file starts with "%s"', magic);
info = dir(p);
assert(info.bytes > 100);
rc.select('none');
rc.close();
clear cleaner
rmdir(d, 's');
end
