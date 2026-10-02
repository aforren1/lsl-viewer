function out = run_spawn(args)
% RUN_SPAWN  Run tests/spawn.py with a cell array of arguments; return its output.
%   py = run_spawn() returns the Python command: LSLRC_PYTHON, or python/python3.
py = getenv('LSLRC_PYTHON');
if isempty(py)
  if ispc, py = 'python'; else py = 'python3'; end
end
if nargin < 1, out = py; return; end
here = fileparts(mfilename('fullpath'));
cmd = [q(py) ' ' q(fullfile(here, 'spawn.py'))];
for i = 1:numel(args), cmd = [cmd ' ' q(args{i})]; end %#ok<AGROW>
% cmd.exe removes the first and the last quote when the line starts with a quote.
if ispc && cmd(1) == '"', cmd = ['"' cmd '"']; end
[st, out] = system(cmd);
if st ~= 0, error('lslrc:test', 'spawn.py failed (%d): %s', st, out); end
end

function s = q(s)
if any(s == ' ' | s == '|' | s == '&' | s == '"'), s = ['"' s '"']; end
end
