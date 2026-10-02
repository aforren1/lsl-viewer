classdef Client < handle
  % lslrc.Client  Client for the lsl-viewer TCP remote control (protocol 2).
  %
  %   rc = lslrc.Client()                      connect to 127.0.0.1:22345
  %   rc = lslrc.Client(host, port, 'Timeout', 5, 'GetTimeout', 60, 'Transport', 'auto')
  %
  % See README.md for the API. Error identifiers: lslrc:connect, lslrc:protocol,
  % lslrc:busy, lslrc:remote, lslrc:timeout, lslrc:closed, lslrc:transport,
  % lslrc:file, lslrc:arg.

  properties (SetAccess = private)
    host = '127.0.0.1';
    port = 22345;
    protocol = 0;     % from the banner
    banner = '';
    transport = '';   % 'java' or 'tcpclient'
    Timeout = 5;      % s; must exceed the ~2 s that select/start/stop can block
    GetTimeout = 60;  % s without data during get; the server can wait ~3 s to flush
  end

  properties (Access = private)
    sock = [];        % java.net.Socket or tcpclient
    in = [];          % Java channel over a buffered socket stream
    out = [];
    small = [];       % Java ByteBuffers: small for lines, big for get bodies
    big = [];
    avail = '';       % tcpclient byte-count property: the name changed in R2020b
    buf = uint8([]);  % bytes read but not used yet; a header and data can share a segment
  end

  methods
    function obj = Client(host, port, varargin)
      if nargin >= 1 && ~isempty(host), obj.host = host; end
      if nargin >= 2 && ~isempty(port), obj.port = port; end
      kind = 'auto';
      if mod(numel(varargin), 2), error('lslrc:arg', 'Options must be name/value pairs.'); end
      for i = 1:2:numel(varargin)
        switch lower(varargin{i})
          case 'timeout', obj.Timeout = varargin{i+1};
          case 'gettimeout', obj.GetTimeout = varargin{i+1};
          case 'transport', kind = lower(varargin{i+1});
          otherwise, error('lslrc:arg', 'Unknown option "%s".', varargin{i});
        end
      end
      obj.open(kind);
      try
        obj.banner = obj.readline();
        if strncmp(obj.banner, 'error:', 6)
          if ~isempty(strfind(obj.banner, 'too many'))
            error('lslrc:busy', 'The viewer refused the connection: %s', obj.banner);
          end
          error('lslrc:remote', '%s', obj.banner);
        end
        tok = regexp(obj.banner, 'protocol (\d+)', 'tokens', 'once');
        if isempty(tok)
          error('lslrc:protocol', ['The server is too old (protocol 1, banner "%s"). ' ...
                'Update lsl-viewer to a version with protocol 2 or later.'], obj.banner);
        end
        obj.protocol = str2double(tok{1});
        if obj.protocol < 2
          error('lslrc:protocol', 'The server speaks protocol %d; this client needs 2 or later.', obj.protocol);
        end
      catch err
        obj.close();
        rethrow(err);
      end
    end

    function delete(obj)
      obj.close();
    end

    function close(obj)
      try
        if strcmp(obj.transport, 'java') && ~isempty(obj.sock), obj.sock.close(); end
      catch %#ok<CTCH> closing a dead socket must not throw from a destructor
      end
      obj.sock = []; obj.in = []; obj.out = []; obj.buf = uint8([]);
    end

    function r = command(obj, line)
      % Send one line; return the whole reply as text, without a check for "error:".
      r = strjoin(obj.request(line), char(10));
    end

    function s = status(obj)
      line = obj.ok('status');
      k = strfind(line, 'file=');
      if ~strncmp(line, 'ok: ', 4) || isempty(k)
        error('lslrc:protocol', 'Unexpected status reply: %s', line);
      end
      % The path can contain spaces or "=", thus split only the part before "file=".
      s = struct('recording', false, 'seconds', NaN, 'streams', NaN, 'bytes', NaN, ...
                 'file', line(k(1)+5:end));
      toks = strsplit(strtrim(line(5:k(1)-1)), ' ');
      for i = 1:numel(toks)
        kv = regexp(toks{i}, '^(\w+)=(.*)$', 'tokens', 'once');
        if isempty(kv), continue; end
        if strcmp(kv{1}, 'recording')
          s.recording = strcmp(kv{2}, 'true');
        elseif any(strcmp(kv{1}, {'seconds', 'streams', 'bytes'}))
          s.(kv{1}) = str2double(kv{2});
        end
      end
    end

    function st = streams(obj)
      lines = obj.counted('streams');
      st = struct('key', {}, 'name', {}, 'type', {}, 'channels', {}, 'rate', {}, 'recording', {});
      for i = 1:numel(lines)
        f = strsplit(lines{i}, ' | ', 'CollapseDelimiters', false);
        if numel(f) < 5, error('lslrc:protocol', 'Unexpected stream line: %s', lines{i}); end
        rate = strtrim(f{end});
        rec = ~isempty(regexp(rate, '\[rec\]$', 'once'));
        rate = strtrim(regexprep(rate, '\[rec\]$', ''));
        if strcmp(rate, 'irregular'), rate = 0; else rate = str2double(rate); end
        % A name may contain " | "; the key never does, and the last three fields are fixed.
        st(end+1) = struct('key', strtrim(f{1}), 'name', strjoin(f(2:end-3), ' | '), ...
          'type', strtrim(f{end-2}), 'channels', str2double(regexprep(f{end-1}, 'ch\s*$', '')), ...
          'rate', rate, 'recording', rec); %#ok<AGROW>
      end
    end

    function k = selected(obj)
      k = reshape(obj.counted('selected'), [], 1);
    end

    function r = select(obj, keys)
      % 'all', 'none', or a cellstr of keys from streams().
      if iscell(keys)
        % The server splits on "," and trims spaces, so such a key cannot round-trip.
        bad = ~cellfun(@(k) ischar(k) && ~isempty(strtrim(k)) && strcmp(k, strtrim(k)) && ~any(k == ','), keys);
        if any(bad(:)), error('lslrc:arg', 'Key cannot be sent to the viewer: "%s"', keys{find(bad, 1)}); end
        if isempty(keys), keys = 'none'; else keys = strjoin(keys(:).', ','); end
      end
      r = obj.ok(['select ' keys]);
    end

    function r = set(obj, field, value)
      if isnumeric(value), value = num2str(value); end
      r = obj.ok(['set ' field ' ' value]);
    end

    function r = filename(obj, path)
      r = obj.ok(['filename ' path]);
    end

    function r = start(obj, path)
      if nargin < 2 || isempty(path), r = obj.ok('start'); else r = obj.ok(['start ' path]); end
    end

    function r = stop(obj)
      r = obj.ok('stop');
    end

    function p = get(obj, dest)
      % Save the last completed recording. dest is a folder (keep the server's file
      % name) or a file path. Returns the path of the saved file.
      if nargin < 2 || isempty(dest), dest = pwd; end
      obj.send('get');
      head = obj.readline(obj.GetTimeout);
      if strncmp(head, 'error:', 6), error('lslrc:remote', '%s', head); end
      tok = regexp(head, '^ok: (\d+) (.+)$', 'tokens', 'once');
      if isempty(tok), error('lslrc:protocol', 'Unexpected get header: %s', head); end
      n = str2double(tok{1});
      % Use only the base name; a path from the server must not pick the folder.
      name = regexprep(tok{2}, '^.*[\\/]', '');
      if exist(dest, 'dir') == 7
        if any(strcmp(name, {'', '.', '..'})), error('lslrc:protocol', 'Bad file name: %s', tok{2}); end
        p = fullfile(dest, name);
      else
        p = dest;
      end
      tmp = [p '.part'];
      fid = fopen(tmp, 'wb');
      if fid < 0
        obj.close();  % the unread body makes the connection useless
        error('lslrc:file', 'Cannot write %s', tmp);
      end
      try
        m = min(n, numel(obj.buf));
        fwrite(fid, obj.buf(1:m), 'uint8');
        obj.buf = obj.buf(m+1:end);
        got = m;
        while got < n
          d = obj.recv(min(n - got, 1048576), true, obj.GetTimeout);
          fwrite(fid, d, 'uint8');
          got = got + numel(d);
        end
        fclose(fid); fid = -1;
        info = dir(tmp);
        if numel(info) ~= 1 || info.bytes ~= n
          error('lslrc:file', 'The size of %s is not %d bytes.', tmp, n);
        end
        [ok, msg] = movefile(tmp, p, 'f');
        if ~ok, error('lslrc:file', 'Cannot move %s to %s: %s', tmp, p, msg); end
      catch err
        if fid >= 0, fclose(fid); end
        if exist(tmp, 'file'), delete(tmp); end
        obj.close();
        rethrow(err);
      end
    end
  end

  methods (Access = private)
    function c = request(obj, line)
      % Return the reply as a cell row: the first line, then the counted lines.
      verb = lower(strtok(line));
      if strcmp(verb, 'get'), error('lslrc:arg', 'Use the get method to receive a file.'); end
      obj.send(line);
      c = {obj.readline()};
      % Read the counted body now, otherwise it would be taken as the next reply.
      n = regexp(c{1}, '^ok: (\d+) ', 'tokens', 'once');
      if any(strcmp(verb, {'streams', 'selected', 'help'})) && ~isempty(n)
        c = [c, cell(1, str2double(n{1}))];
        for i = 2:numel(c), c{i} = obj.readline(); end
      end
      if any(strcmp(verb, {'quit', 'exit'})), obj.close(); end
    end

    function r = ok(obj, line)
      r = obj.command(line);
      if strncmp(r, 'error:', 6), error('lslrc:remote', '%s', r); end
    end

    function lines = counted(obj, verb)
      c = obj.request(verb);
      if strncmp(c{1}, 'error:', 6), error('lslrc:remote', '%s', c{1}); end
      if isempty(regexp(c{1}, '^ok: \d+ ', 'once'))
        error('lslrc:protocol', 'Unexpected %s header: %s', verb, c{1});
      end
      lines = c(2:end);
    end

    function send(obj, line)
      if any(line == 10 | line == 13), error('lslrc:arg', 'A command must be one line.'); end
      if isempty(obj.sock), error('lslrc:closed', 'The connection is closed.'); end
      b = unicode2native([line char(10)], 'UTF-8');
      try
        if strcmp(obj.transport, 'java')
          obj.out.write(typecast(uint8(b), 'int8'));
          obj.out.flush();
        else
          write(obj.sock, uint8(b));
        end
      catch err
        obj.close();
        error('lslrc:closed', 'Cannot send to the viewer: %s', brief(err));
      end
    end

    function line = readline(obj, timeout)
      if nargin < 2, timeout = obj.Timeout; end
      from = 1;
      i = [];
      while isempty(i)
        i = find(obj.buf(from:end) == 10, 1);
        if isempty(i)
          from = numel(obj.buf) + 1;
          obj.buf = [obj.buf, obj.recv(16384, false, timeout)];
        end
      end
      i = i + from - 1;
      b = obj.buf(1:i-1);
      obj.buf = obj.buf(i+1:end);
      if ~isempty(b) && b(end) == 13, b = b(1:end-1); end
      if isempty(b), line = ''; else line = native2unicode(b, 'UTF-8'); end
    end

    function open(obj, kind)
      if strcmp(kind, 'auto')
        if usejava('jvm')
          kind = 'java';
        elseif loadtcp()
          kind = 'tcpclient';
        else
          error('lslrc:transport', ['No TCP transport. Start MATLAB with Java, or in Octave ' ...
                'install a JVM or the instrument-control package (pkg install -forge instrument-control).']);
        end
      elseif strcmp(kind, 'tcpclient') && ~loadtcp()
        error('lslrc:transport', 'tcpclient is not available.');
      elseif ~any(strcmp(kind, {'java', 'tcpclient'}))
        error('lslrc:arg', 'Transport must be ''auto'', ''java'', or ''tcpclient''.');
      end
      obj.transport = kind;
      try
        if strcmp(kind, 'java')
          s = javaObject('java.net.Socket');
          s.connect(javaObject('java.net.InetSocketAddress', obj.host, obj.port), ...
                    round(1000 * obj.Timeout));
          s.setTcpNoDelay(true);
          obj.in = javaMethod('newChannel', 'java.nio.channels.Channels', ...
                     javaObject('java.io.BufferedInputStream', s.getInputStream(), 65536));
          obj.out = s.getOutputStream();
          obj.small = javaMethod('allocate', 'java.nio.ByteBuffer', 16384);
          obj.big = javaMethod('allocate', 'java.nio.ByteBuffer', 1048576);
        else
          s = tcpclient(obj.host, obj.port, 'Timeout', obj.Timeout);
          try
            s.NumBytesAvailable;
            obj.avail = 'NumBytesAvailable';
          catch %#ok<CTCH> MATLAB before R2020b
            obj.avail = 'BytesAvailable';
          end
        end
      catch err
        error('lslrc:connect', 'Cannot connect to %s:%d: %s', obj.host, obj.port, brief(err));
      end
      obj.sock = s;
    end

    function d = recv(obj, n, exact, timeout)
      % Return a uint8 row: exactly n bytes, or (exact false) 1 to n bytes.
      if isempty(obj.sock), error('lslrc:closed', 'The connection is closed.'); end
      try
        if strcmp(obj.transport, 'java')
          d = obj.jrecv(n, exact, timeout);
        else
          d = obj.trecv(n, exact, timeout);
        end
      catch err
        obj.close();
        if any(strcmp(err.identifier, {'lslrc:timeout', 'lslrc:closed'})), rethrow(err); end
        if ~isempty(regexpi(err.message, 'timed? ?out|timeout', 'once'))
          error('lslrc:timeout', 'No reply from the viewer in %g s.', timeout);
        end
        error('lslrc:closed', 'The connection failed: %s', brief(err));
      end
    end

    function d = jrecv(obj, n, exact, timeout)
      obj.sock.setSoTimeout(round(1000 * timeout));
      if n > obj.small.capacity(), bb = obj.big; else bb = obj.small; end
      n = min(n, bb.capacity());
      bb.clear();
      bb.limit(n);
      k = 0;
      % Fill the buffer before one conversion: each Java-to-array copy has a high fixed cost.
      while k == 0 || (exact && k < n)
        r = obj.in.read(bb);
        if r < 0, error('lslrc:closed', 'The viewer closed the connection.'); end
        k = k + r;
      end
      a = bb.array();
      d = reshape(typecast(a(1:k), 'uint8'), 1, []);
    end

    function d = trecv(obj, n, exact, timeout)
      if obj.sock.Timeout ~= timeout, obj.sock.Timeout = timeout; end
      if exact
        d = read(obj.sock, n, 'uint8');
      else
        k = obj.sock.(obj.avail);
        if k == 0
          % Block for one byte, then take what came with it.
          d = read(obj.sock, 1, 'uint8');
          k = obj.sock.(obj.avail);
          if ~isempty(d) && k > 0 && n > 1, d = [d(:).', reshape(read(obj.sock, min(k, n - 1), 'uint8'), 1, [])]; end
        else
          d = read(obj.sock, min(k, n), 'uint8');
        end
      end
      % Octave returns short data on a timeout; MATLAB throws.
      if isempty(d) || (exact && numel(d) < n)
        error('lslrc:timeout', 'No reply from the viewer in %g s.', timeout);
      end
      d = reshape(d, 1, []);
    end
  end
end

function ok = loadtcp()
  ok = exist('tcpclient') ~= 0; %#ok<EXIST>
  if ~ok && exist('OCTAVE_VERSION', 'builtin')
    try
      pkg('load', 'instrument-control');
      ok = exist('tcpclient') ~= 0; %#ok<EXIST>
    catch %#ok<CTCH>
      ok = false;
    end
  end
end

function m = brief(err)
  % A Java exception message carries a stack trace; keep only the exception line.
  m = regexp(err.message, 'java\.[\w.$]+: [^\r\n]*', 'match', 'once');
  if isempty(m), m = err.message; end
end
