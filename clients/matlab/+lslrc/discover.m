function v = discover(varargin)
% lslrc.discover  Find lsl-viewer control ports through LSL. Needs liblsl-Matlab.
%
%   v = lslrc.discover()                         wait up to 2 s for 1 viewer
%   v = lslrc.discover('Timeout', 5, 'Minimum', 100)  wait 5 s, collect all viewers
%   rc = lslrc.Client(v(1).host, v(1).port);
%
% v is a struct array with the fields host (the address to connect to), port, pid,
% bind, protocol (NaN if the viewer does not announce it), hostname, and source_id.
timeout = 2;
minimum = 1;
for i = 1:2:numel(varargin)
  switch lower(varargin{i})
    case 'timeout', timeout = varargin{i+1};
    case 'minimum', minimum = varargin{i+1};
    otherwise, error('lslrc:arg', 'Unknown option "%s".', varargin{i});
  end
end
if exist('lsl_loadlib') == 0 %#ok<EXIST>
  error('lslrc:nolsl', ['lslrc.discover needs liblsl-Matlab on the path (lsl_loadlib). ' ...
        'Get it from https://github.com/labstreaminglayer/liblsl-Matlab, or give the ' ...
        'host and the port to lslrc.Client directly.']);
end
lib = lsl_loadlib();
found = lsl_resolve_byprop(lib, 'type', 'ViewerControl', minimum, timeout);
v = struct('host', {}, 'port', {}, 'pid', {}, 'bind', {}, 'protocol', {}, ...
           'hostname', {}, 'source_id', {});
for i = 1:numel(found)
  info = found{i};
  sid = info.source_id();
  % The source_id ends in ":<port>", thus the port is known without the description.
  e = struct('host', info.hostname(), 'port', str2double(regexp(sid, '\d+$', 'match', 'once')), ...
             'pid', NaN, 'bind', '', 'protocol', NaN, 'hostname', info.hostname(), 'source_id', sid);
  try
    % The description is only in the full info, which comes through an inlet.
    inlet = lsl_inlet(info);
    full = inlet.info(3);
    desc = full.desc();
    e.bind = desc.child_value('bind');
    e.pid = str2double(desc.child_value('pid'));
    e.protocol = str2double(desc.child_value('protocol_version'));
    p = str2double(desc.child_value('port'));
    if ~isnan(p), e.port = p; end
  catch %#ok<CTCH> keep what the source_id gave
  end
  % A loopback-only viewer is reachable at 127.0.0.1, not at its machine name.
  if strcmp(e.bind, 'loopback'), e.host = '127.0.0.1'; end
  v(end+1) = e; %#ok<AGROW>
end
end
