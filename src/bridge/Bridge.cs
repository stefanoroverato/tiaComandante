// TiaComandante.Bridge - generic reflection bridge between native C and .NET.
//
// Contains no TIA Portal domain logic and references no Siemens assembly at
// compile time: everything is resolved at runtime by name. Written in C# 5 so
// that it builds with the csc.exe shipped with .NET Framework 4.8.
//
// Native contract (x64):
//   Entry.Init(string) is called through ICLRRuntimeHost::ExecuteInDefaultAppDomain
//   with "api=<hex>\ncb=<hex>\ndir=<openness dir>". It fills the native struct
//       struct { uint32 size; uint32 version; invoke_fn invoke; free_fn free_str; }
//   invoke(const char* req_utf8, char** resp_utf8) -> 0 on success.
//   Strings returned to native are allocated with CoTaskMemAlloc.
//   The native callback has signature
//       int cb(int64 id, const char* args_utf8, char** result_utf8)
//   and returns a CoTaskMemAlloc'ed result (or NULL).

using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Linq.Expressions;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Web.Script.Serialization;

namespace TiaComandante.Bridge
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int InvokeFn(IntPtr req, out IntPtr resp);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void FreeFn(IntPtr p);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int NativeCallbackFn(long id, IntPtr args, out IntPtr result);

    internal sealed class RefEq : IEqualityComparer<object>
    {
        public new bool Equals(object a, object b) { return ReferenceEquals(a, b); }
        public int GetHashCode(object o) { return System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(o); }
    }

    internal sealed class BridgeException : Exception
    {
        public BridgeException(string msg) : base(msg) { }
    }

    public static class Entry
    {
        public const int Version = 1;

        static InvokeFn s_invoke;
        static FreeFn s_free;
        static NativeCallbackFn s_native;
        static string s_opennessDir;
        static bool s_allLoaded;
        // One serializer per thread: TIA Portal events run on Openness threads, in parallel with requests.
        [ThreadStatic] static JavaScriptSerializer t_json;
        static JavaScriptSerializer Json { get { return t_json ?? (t_json = CreateSerializer()); } }

        // ---- handle table -------------------------------------------------
        sealed class Slot { public object Obj; public bool Pinned; }
        static readonly object s_lock = new object();
        static readonly Dictionary<long, Slot> s_byId = new Dictionary<long, Slot>();
        static readonly Dictionary<object, long> s_byObj = new Dictionary<object, long>(new RefEq());
        static long s_nextId = 1;
        [ThreadStatic] static List<List<long>> t_scopes;

        static readonly Dictionary<string, Type> s_typeCache = new Dictionary<string, Type>();
        static readonly Dictionary<long, KeyValuePair<object, Delegate>> s_subs = new Dictionary<long, KeyValuePair<object, Delegate>>();
        static readonly Dictionary<long, EventInfo> s_subEvents = new Dictionary<long, EventInfo>();
        static long s_nextSub = 1;

        static JavaScriptSerializer CreateSerializer()
        {
            var js = new JavaScriptSerializer();
            js.MaxJsonLength = int.MaxValue;
            js.RecursionLimit = 256;
            return js;
        }

        // ---- bootstrap ----------------------------------------------------
        public static int Init(string arg)
        {
            try
            {
                long api = 0, cb = 0;
                foreach (string line in arg.Split('\n'))
                {
                    int eq = line.IndexOf('=');
                    if (eq <= 0) continue;
                    string k = line.Substring(0, eq).Trim();
                    string v = line.Substring(eq + 1).Trim();
                    if (k == "api") api = long.Parse(v, NumberStyles.HexNumber);
                    else if (k == "cb") cb = long.Parse(v, NumberStyles.HexNumber);
                    else if (k == "dir") s_opennessDir = v;
                }
                if (api == 0) return 2;

                AppDomain.CurrentDomain.AssemblyResolve += OnAssemblyResolve;

                // stdout carries the MCP protocol: nothing loaded in-process may write to the console.
                // Native code can route the text to its log with the "console" op.
                Console.SetOut(TextWriter.Null);
                Console.SetError(TextWriter.Null);

                s_invoke = new InvokeFn(Invoke);
                s_free = new FreeFn(Free);
                if (cb != 0)
                    s_native = (NativeCallbackFn)Marshal.GetDelegateForFunctionPointer(new IntPtr(cb), typeof(NativeCallbackFn));

                IntPtr p = new IntPtr(api);
                Marshal.WriteInt32(p, 4, Version);
                Marshal.WriteIntPtr(p, 8, Marshal.GetFunctionPointerForDelegate(s_invoke));
                Marshal.WriteIntPtr(p, 16, Marshal.GetFunctionPointerForDelegate(s_free));
                return 0;
            }
            catch
            {
                return 1;
            }
        }

        static Assembly OnAssemblyResolve(object sender, ResolveEventArgs args)
        {
            string name = new AssemblyName(args.Name).Name;
            // Assemblies loaded with the "load" op live in the LoadFrom context: resolve them by name.
            foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies())
                if (string.Equals(a.GetName().Name, name, StringComparison.OrdinalIgnoreCase)) return a;
            if (string.IsNullOrEmpty(s_opennessDir)) return null;
            if (!name.StartsWith("Siemens.Engineering", StringComparison.OrdinalIgnoreCase)) return null;
            string path = Path.Combine(s_opennessDir, name + ".dll");
            return File.Exists(path) ? Assembly.LoadFrom(path) : null;
        }

        static void LoadAllOpenness()
        {
            if (s_allLoaded || string.IsNullOrEmpty(s_opennessDir) || !Directory.Exists(s_opennessDir)) return;
            s_allLoaded = true;
            foreach (string f in Directory.GetFiles(s_opennessDir, "Siemens.Engineering*.dll"))
            {
                try { Assembly.LoadFrom(f); } catch { }
            }
        }

        // ---- native entry points -----------------------------------------
        static void Free(IntPtr p)
        {
            if (p != IntPtr.Zero) Marshal.FreeCoTaskMem(p);
        }

        static int Invoke(IntPtr req, out IntPtr resp)
        {
            object result;
            int rc = 0;
            try
            {
                var r = Json.DeserializeObject(ReadUtf8(req)) as Dictionary<string, object>;
                if (r == null) throw new BridgeException("request must be a JSON object");
                var ok = new Dictionary<string, object>();
                ok["ok"] = true;
                ok["r"] = Dispatch(r);
                result = ok;
            }
            catch (Exception ex)
            {
                rc = 1;
                var err = new Dictionary<string, object>();
                err["ok"] = false;
                err["err"] = DescribeException(ex);
                result = err;
            }
            string text;
            try { text = Json.Serialize(result); }
            catch (Exception ex)
            {
                rc = 1;
                var err = new Dictionary<string, object>();
                err["ok"] = false;
                err["err"] = DescribeException(ex);
                text = Json.Serialize(err);
            }
            resp = WriteUtf8(text);
            return rc;
        }

        static string ReadUtf8(IntPtr p)
        {
            if (p == IntPtr.Zero) return "";
            int len = 0;
            while (Marshal.ReadByte(p, len) != 0) len++;
            byte[] buf = new byte[len];
            Marshal.Copy(p, buf, 0, len);
            return Encoding.UTF8.GetString(buf);
        }

        static IntPtr WriteUtf8(string s)
        {
            byte[] buf = Encoding.UTF8.GetBytes(s ?? "");
            IntPtr p = Marshal.AllocCoTaskMem(buf.Length + 1);
            Marshal.Copy(buf, 0, p, buf.Length);
            Marshal.WriteByte(p, buf.Length, 0);
            return p;
        }

        static Exception Unwrap(Exception ex)
        {
            while (ex is TargetInvocationException && ex.InnerException != null) ex = ex.InnerException;
            return ex;
        }

        static Dictionary<string, object> DescribeException(Exception ex)
        {
            ex = Unwrap(ex);
            var d = new Dictionary<string, object>();
            d["type"] = ex.GetType().FullName;
            d["message"] = ex.Message;
            d["hresult"] = ex.HResult;
            var inner = new List<object>();
            for (Exception i = ex.InnerException; i != null && inner.Count < 8; i = i.InnerException)
            {
                var id = new Dictionary<string, object>();
                id["type"] = i.GetType().FullName;
                id["message"] = i.Message;
                inner.Add(id);
            }
            d["inner"] = inner;
            if (ex is BridgeException) d["bridge"] = true;
            return d;
        }

        // ---- dispatcher ----------------------------------------------------
        static object Dispatch(Dictionary<string, object> r)
        {
            string op = Str(r, "op");
            switch (op)
            {
                case "ping": return "pong";
                case "get": return ToWire(GetMember(Target(r), Str(r, "name")));
                case "set": SetMember(Target(r), Str(r, "name"), Val(r, "value")); return null;
                case "call": return Call(Target(r), null, r);
                case "static": return Call(null, ResolveTypeOrThrow(Str(r, "type")), r);
                case "new": return ToWire(Construct(ConstructedType(r), Args(r), Sig(r)));
                case "service": return ToWire(GetService(Target(r), ResolveTypeOrThrow(Str(r, "type"))));
                case "enumerate": return Enumerate(Target(r), StrList(r, "attrs"), IntOr(r, "limit", -1));
                case "attrs": return ReadAttrs(Target(r), StrList(r, "attrs"));
                case "is": return IsInstance(Val(r, "h"), Str(r, "type"));
                case "type": return DescribeType(Target(r));
                case "members": return ListMembers(r.ContainsKey("h") ? Target(r).GetType() : ResolveTypeOrThrow(Str(r, "type")));
                case "tostring": { object t = Target(r); return t == null ? null : t.ToString(); }
                case "equals": return Equals(Target(r), Resolve(Val(r, "other")));
                case "count": lock (s_lock) return s_byId.Count;
                case "subscribe": return Subscribe(Target(r), Str(r, "name"), LongOf(Val(r, "cb")));
                case "unsubscribe": Unsubscribe(LongOf(Val(r, "sub"))); return null;
                case "delegate": return ToWire(MakeDelegate(ResolveTypeOrThrow(Str(r, "type")), LongOf(Val(r, "cb"))));
                case "pin": SetPinned(LongOf(Val(r, "h")), true); return null;
                case "unpin": SetPinned(LongOf(Val(r, "h")), false); return null;
                case "release": Release(LongOf(Val(r, "h"))); return null;
                case "scope_begin": ScopeBegin(); return null;
                case "scope_end": ScopeEnd(); return null;
                case "load": return ToWire(Assembly.LoadFrom(Str(r, "path")));
                case "console": SetConsole(r.ContainsKey("cb") && r["cb"] != null ? LongOf(r["cb"]) : 0); return null;
                default: throw new BridgeException("unknown op '" + op + "'");
            }
        }

        // call/static: with "outs": true, trailing out parameters may be omitted and the
        // result is {"ret": value, "out": [by-ref parameter values in order]}.
        static object Call(object target, Type staticType, Dictionary<string, object> r)
        {
            object o;
            bool outs = r.TryGetValue("outs", out o) && o is bool && (bool)o;
            MethodBase m;
            object[] conv;
            object ret = CallMethod(target, staticType, Str(r, "name"), Args(r), Generic(r), Sig(r), outs, out m, out conv);
            if (!outs) return ToWire(ret);
            var d = new Dictionary<string, object>();
            d["ret"] = ToWire(ret);
            var list = new List<object>();
            if (m != null)
            {
                ParameterInfo[] ps = m.GetParameters();
                for (int i = 0; i < ps.Length; i++)
                    if (ps[i].ParameterType.IsByRef) list.Add(ToWire(conv[i]));
            }
            d["out"] = list;
            return d;
        }

        // new: "generic" closes a generic type definition, e.g. Dictionary`2 with [UInt64, X].
        static Type ConstructedType(Dictionary<string, object> r)
        {
            Type t = ResolveTypeOrThrow(Str(r, "type"));
            Type[] g = Generic(r);
            return g == null ? t : t.MakeGenericType(g);
        }

        // Console text written by loaded assemblies goes to native callback cb (args: [line]), or nowhere (cb = 0).
        sealed class CallbackWriter : TextWriter
        {
            readonly long m_cb;
            readonly StringBuilder m_line = new StringBuilder();
            public CallbackWriter(long cb) { m_cb = cb; }
            public override Encoding Encoding { get { return Encoding.UTF8; } }
            public override void Write(char value)
            {
                if (value == '\n')
                {
                    string s = m_line.ToString().TrimEnd('\r');
                    m_line.Length = 0;
                    DispatchCallback(m_cb, new object[] { s }, typeof(void));
                }
                else if (m_line.Length < 8192) m_line.Append(value);
            }
        }

        static void SetConsole(long cb)
        {
            TextWriter w = cb == 0 ? TextWriter.Null : TextWriter.Synchronized(new CallbackWriter(cb));
            Console.SetOut(w);
            Console.SetError(w);
        }

        static string Str(Dictionary<string, object> r, string k)
        {
            object v;
            if (!r.TryGetValue(k, out v) || v == null) throw new BridgeException("missing '" + k + "'");
            return Convert.ToString(v, CultureInfo.InvariantCulture);
        }

        static object Val(Dictionary<string, object> r, string k)
        {
            object v;
            return r.TryGetValue(k, out v) ? v : null;
        }

        static int IntOr(Dictionary<string, object> r, string k, int def)
        {
            object v;
            return r.TryGetValue(k, out v) && v != null ? Convert.ToInt32(v, CultureInfo.InvariantCulture) : def;
        }

        static List<string> StrList(Dictionary<string, object> r, string k)
        {
            object v;
            var res = new List<string>();
            if (!r.TryGetValue(k, out v) || v == null) return res;
            var arr = v as object[];
            if (arr != null) { foreach (object o in arr) res.Add(Convert.ToString(o, CultureInfo.InvariantCulture)); }
            else
            {
                foreach (string s in Convert.ToString(v, CultureInfo.InvariantCulture).Split(','))
                    if (s.Trim().Length > 0) res.Add(s.Trim());
            }
            return res;
        }

        static object[] Args(Dictionary<string, object> r)
        {
            object v;
            if (!r.TryGetValue("args", out v) || v == null) return new object[0];
            var arr = v as object[];
            if (arr == null) throw new BridgeException("'args' must be an array");
            return arr;
        }

        static Type[] Generic(Dictionary<string, object> r)
        {
            var names = StrList(r, "generic");
            if (names.Count == 0) return null;
            return names.Select(ResolveTypeOrThrow).ToArray();
        }

        static List<string> Sig(Dictionary<string, object> r)
        {
            return r.ContainsKey("sig") ? StrList(r, "sig") : null;
        }

        static long LongOf(object v)
        {
            if (v == null) throw new BridgeException("missing handle/id");
            var d = v as Dictionary<string, object>;
            if (d != null && d.ContainsKey("$h")) return Convert.ToInt64(d["$h"], CultureInfo.InvariantCulture);
            return Convert.ToInt64(v, CultureInfo.InvariantCulture);
        }

        static object Target(Dictionary<string, object> r)
        {
            object v = Val(r, "h");
            if (v == null) throw new BridgeException("missing 'h'");
            object o = Lookup(LongOf(v));
            if (o == null) throw new BridgeException("null target");
            return o;
        }

        // ---- handles -------------------------------------------------------
        static long Register(object o)
        {
            lock (s_lock)
            {
                long id;
                if (!o.GetType().IsValueType && s_byObj.TryGetValue(o, out id)) return id;
                id = s_nextId++;
                s_byId[id] = new Slot { Obj = o };
                if (!o.GetType().IsValueType) s_byObj[o] = id;
                if (t_scopes != null && t_scopes.Count > 0) t_scopes[t_scopes.Count - 1].Add(id);
                return id;
            }
        }

        static object Lookup(long id)
        {
            if (id == 0) return null;
            lock (s_lock)
            {
                Slot s;
                if (!s_byId.TryGetValue(id, out s)) throw new BridgeException("invalid or released handle " + id);
                return s.Obj;
            }
        }

        static void SetPinned(long id, bool pinned)
        {
            lock (s_lock)
            {
                Slot s;
                if (s_byId.TryGetValue(id, out s)) s.Pinned = pinned;
            }
        }

        static void Release(long id)
        {
            lock (s_lock) ReleaseLocked(id, true);
        }

        static void ReleaseLocked(long id, bool force)
        {
            Slot s;
            if (!s_byId.TryGetValue(id, out s)) return;
            if (s.Pinned && !force) return;
            s_byId.Remove(id);
            if (s.Obj != null && !s.Obj.GetType().IsValueType) s_byObj.Remove(s.Obj);
        }

        static void ScopeBegin()
        {
            if (t_scopes == null) t_scopes = new List<List<long>>();
            t_scopes.Add(new List<long>());
        }

        static void ScopeEnd()
        {
            if (t_scopes == null || t_scopes.Count == 0) return;
            List<long> ids = t_scopes[t_scopes.Count - 1];
            t_scopes.RemoveAt(t_scopes.Count - 1);
            lock (s_lock) foreach (long id in ids) ReleaseLocked(id, false);
        }

        // ---- wire conversion -------------------------------------------------
        static object ToWire(object v)
        {
            if (v == null) return null;
            if (v is string || v is bool) return v;
            if (v is char) return v.ToString();
            Type t = v.GetType();
            if (t.IsEnum)
            {
                var d = new Dictionary<string, object>();
                d["$enum"] = t.FullName;
                d["name"] = v.ToString();
                d["value"] = Convert.ToInt64(v, CultureInfo.InvariantCulture);
                return d;
            }
            if (v is sbyte || v is byte || v is short || v is ushort || v is int || v is uint)
                return Convert.ToInt64(v, CultureInfo.InvariantCulture);
            // Integers beyond 2^53 travel as digit strings: JSON numbers are doubles on the native side.
            if (v is long)
            {
                long l = (long)v;
                return l > MaxExact || l < -MaxExact ? Marker("$i64", l.ToString(CultureInfo.InvariantCulture)) : (object)l;
            }
            if (v is ulong)
            {
                ulong u = (ulong)v;
                return u > (ulong)MaxExact ? Marker("$u64", u.ToString(CultureInfo.InvariantCulture)) : (object)(long)u;
            }
            if (v is float || v is double) return Convert.ToDouble(v, CultureInfo.InvariantCulture);
            if (v is decimal) return Convert.ToDouble(v, CultureInfo.InvariantCulture);
            if (v is DateTime) return ((DateTime)v).ToString("o", CultureInfo.InvariantCulture);
            if (v is DateTimeOffset) return ((DateTimeOffset)v).ToString("o", CultureInfo.InvariantCulture);
            if (v is TimeSpan || v is Guid || v is Version) return v.ToString();
            if (v is FileInfo || v is DirectoryInfo || v is CultureInfo)
            {
                var fd = new Dictionary<string, object>();
                if (v is FileInfo) fd["$file"] = ((FileInfo)v).FullName;
                else if (v is DirectoryInfo) fd["$dir"] = ((DirectoryInfo)v).FullName;
                else fd["$culture"] = ((CultureInfo)v).Name;
                return fd;
            }
            var h = new Dictionary<string, object>();
            h["$h"] = Register(v);
            h["$t"] = t.FullName;
            return h;
        }

        const long MaxExact = 9007199254740992; // 2^53

        static Dictionary<string, object> Marker(string key, object value)
        {
            var d = new Dictionary<string, object>();
            d[key] = value;
            return d;
        }

        // Resolve a wire value without a target type (object parameters).
        static object Resolve(object w)
        {
            var d = w as Dictionary<string, object>;
            if (d != null)
            {
                if (d.ContainsKey("$h")) return Lookup(LongOf(d["$h"]));
                if (d.ContainsKey("$enum"))
                {
                    Type et = ResolveTypeOrThrow(Convert.ToString(d["$enum"], CultureInfo.InvariantCulture));
                    return Enum.Parse(et, Convert.ToString(d["name"], CultureInfo.InvariantCulture), true);
                }
                if (d.ContainsKey("$type")) return ResolveTypeOrThrow(Convert.ToString(d["$type"], CultureInfo.InvariantCulture));
                if (d.ContainsKey("$file")) return new FileInfo(Convert.ToString(d["$file"], CultureInfo.InvariantCulture));
                if (d.ContainsKey("$dir")) return new DirectoryInfo(Convert.ToString(d["$dir"], CultureInfo.InvariantCulture));
                if (d.ContainsKey("$culture")) return CultureInfo.GetCultureInfo(Convert.ToString(d["$culture"], CultureInfo.InvariantCulture));
                if (d.ContainsKey("$i32")) return Convert.ToInt32(d["$i32"], CultureInfo.InvariantCulture);
                if (d.ContainsKey("$u32")) return Convert.ToUInt32(d["$u32"], CultureInfo.InvariantCulture);
                if (d.ContainsKey("$i64")) return Convert.ToInt64(d["$i64"], CultureInfo.InvariantCulture);
                if (d.ContainsKey("$u64")) return Convert.ToUInt64(d["$u64"], CultureInfo.InvariantCulture);
                if (d.ContainsKey("$f64")) return Convert.ToDouble(d["$f64"], CultureInfo.InvariantCulture);
                if (d.ContainsKey("$str")) return Convert.ToString(d["$str"], CultureInfo.InvariantCulture);
                if (d.ContainsKey("$cb")) throw new BridgeException("$cb needs a delegate-typed parameter");
                return d;
            }
            if (w is decimal) return Convert.ToDouble(w, CultureInfo.InvariantCulture);
            var arr = w as object[];
            if (arr != null) return arr.Select(Resolve).ToArray();
            return w;
        }

        static bool IsNumeric(Type t)
        {
            switch (Type.GetTypeCode(t))
            {
                case TypeCode.SByte: case TypeCode.Byte: case TypeCode.Int16: case TypeCode.UInt16:
                case TypeCode.Int32: case TypeCode.UInt32: case TypeCode.Int64: case TypeCode.UInt64:
                case TypeCode.Single: case TypeCode.Double: case TypeCode.Decimal:
                    return true;
            }
            return false;
        }

        static Type EnumerableElement(Type t)
        {
            if (t.IsArray) return t.GetElementType();
            if (t.IsGenericType)
            {
                Type g = t.GetGenericTypeDefinition();
                if (g == typeof(IEnumerable<>) || g == typeof(IList<>) || g == typeof(ICollection<>) ||
                    g == typeof(List<>) || g == typeof(IReadOnlyList<>) || g == typeof(IReadOnlyCollection<>))
                    return t.GetGenericArguments()[0];
            }
            return null;
        }

        // Score how well wire value w fits parameter type t: -1 = incompatible.
        static int Score(object w, Type t)
        {
            if (t.IsByRef) t = t.GetElementType();
            Type nt = Nullable.GetUnderlyingType(t);
            if (nt != null) { if (w == null) return 3; t = nt; }
            if (w == null) return t.IsValueType ? -1 : 2;
            var d = w as Dictionary<string, object>;
            if (d != null)
            {
                if (d.ContainsKey("$cb")) return typeof(Delegate).IsAssignableFrom(t) ? 3 : -1;
                if (d.ContainsKey("$h") || d.ContainsKey("$enum") || d.ContainsKey("$type") || d.ContainsKey("$file") ||
                    d.ContainsKey("$dir") || d.ContainsKey("$culture") || d.ContainsKey("$i32") || d.ContainsKey("$u32") ||
                    d.ContainsKey("$i64") || d.ContainsKey("$u64") || d.ContainsKey("$f64") || d.ContainsKey("$str"))
                {
                    object o = Resolve(w);
                    if (o == null) return t.IsValueType ? -1 : 2;
                    if (o.GetType() == t) return 4;
                    if (t.IsInstanceOfType(o)) return t == typeof(object) ? 1 : 3;
                    if (t.IsEnum && IsNumeric(o.GetType())) return 1;
                    if (IsNumeric(t) && IsNumeric(o.GetType())) return 1;
                    return -1;
                }
                if (t == typeof(object)) return 1;
                if (KeyValueElement(t) != null) return 2;
                return -1;
            }
            if (w is string)
            {
                if (t == typeof(string)) return 4;
                if (t == typeof(System.Security.SecureString)) return 3;
                if (t.IsEnum) return IsEnumName(t, (string)w) ? 3 : -1;
                if (t == typeof(FileInfo) || t == typeof(DirectoryInfo)) return 2;
                if (t == typeof(Type)) return ResolveType((string)w) != null ? 2 : -1;
                if (t == typeof(CultureInfo)) return 2;
                if (t == typeof(object)) return 1;
                if (t == typeof(char)) return ((string)w).Length == 1 ? 2 : -1;
                if (t == typeof(Guid) || t == typeof(Version)) return 1;
                if (t == typeof(TimeSpan) || t == typeof(DateTime)) return 1;
                return -1;
            }
            if (w is bool) return t == typeof(bool) ? 4 : (t == typeof(object) ? 1 : -1);
            if (w is int || w is long || w is decimal || w is double)
            {
                bool integral = !(w is decimal || w is double) || Math.Floor(Convert.ToDouble(w)) == Convert.ToDouble(w);
                if (t == typeof(int) || t == typeof(long)) return integral ? 4 : -1;
                if (t == typeof(double) || t == typeof(float) || t == typeof(decimal)) return 3;
                if (IsNumeric(t)) return integral ? 2 : -1;
                if (t.IsEnum) return integral ? 1 : -1;
                if (t == typeof(object)) return 1;
                return -1;
            }
            var arr = w as object[];
            if (arr != null)
            {
                Type el = EnumerableElement(t);
                if (el != null)
                {
                    int s = 3;
                    foreach (object e in arr) { int es = Score(e, el); if (es < 0) return -1; s = Math.Min(s, es + 1); }
                    return s;
                }
                if (t == typeof(object)) return 1;
                return -1;
            }
            return -1;
        }

        static bool IsEnumName(Type t, string s)
        {
            foreach (string part in s.Split(','))
            {
                string p = part.Trim();
                bool found = false;
                foreach (string n in Enum.GetNames(t))
                    if (string.Equals(n, p, StringComparison.OrdinalIgnoreCase)) { found = true; break; }
                if (!found) return false;
            }
            return true;
        }

        static Type KeyValueElement(Type t)
        {
            Type el = EnumerableElement(t);
            if (el != null && el.IsGenericType && el.GetGenericTypeDefinition() == typeof(KeyValuePair<,>)) return el;
            return null;
        }

        // Convert wire value w to an instance of type t.
        static object Convert2(object w, Type t)
        {
            if (t.IsByRef) t = t.GetElementType();
            Type nt = Nullable.GetUnderlyingType(t);
            if (nt != null) { if (w == null) return null; t = nt; }
            if (w == null) return null;
            var d = w as Dictionary<string, object>;
            if (d != null)
            {
                if (d.ContainsKey("$cb")) return MakeDelegate(t, LongOf(d["$cb"]));
                Type kv = KeyValueElement(t);
                if (kv != null && !d.Keys.Any(k => k.StartsWith("$")))
                {
                    Type[] ga = kv.GetGenericArguments();
                    IList list = (IList)Activator.CreateInstance(typeof(List<>).MakeGenericType(kv));
                    foreach (var e in d)
                        list.Add(Activator.CreateInstance(kv, Convert2(e.Key, ga[0]), Convert2(e.Value, ga[1])));
                    return list;
                }
                object o = Resolve(w);
                return o == null ? null : Coerce(o, t);
            }
            var arr = w as object[];
            if (arr != null)
            {
                Type el = EnumerableElement(t);
                if (el == null) return Resolve(w);
                if (t.IsArray)
                {
                    Array a = Array.CreateInstance(el, arr.Length);
                    for (int i = 0; i < arr.Length; i++) a.SetValue(Convert2(arr[i], el), i);
                    return a;
                }
                IList list = (IList)Activator.CreateInstance(typeof(List<>).MakeGenericType(el));
                foreach (object e in arr) list.Add(Convert2(e, el));
                return list;
            }
            var s = w as string;
            if (s != null)
            {
                if (t == typeof(string) || t == typeof(object)) return s;
                if (t == typeof(System.Security.SecureString))
                {
                    var ss = new System.Security.SecureString();
                    foreach (char ch in s) ss.AppendChar(ch);
                    ss.MakeReadOnly();
                    return ss;
                }
                if (t.IsEnum) return Enum.Parse(t, s, true);
                if (t == typeof(FileInfo)) return new FileInfo(s);
                if (t == typeof(DirectoryInfo)) return new DirectoryInfo(s);
                if (t == typeof(Type)) return ResolveTypeOrThrow(s);
                if (t == typeof(CultureInfo)) return CultureInfo.GetCultureInfo(s);
                if (t == typeof(char)) return s[0];
                if (t == typeof(Guid)) return new Guid(s);
                if (t == typeof(Version)) return new Version(s);
                if (t == typeof(TimeSpan)) return TimeSpan.Parse(s, CultureInfo.InvariantCulture);
                if (t == typeof(DateTime)) return DateTime.Parse(s, CultureInfo.InvariantCulture, DateTimeStyles.RoundtripKind);
            }
            if (t == typeof(object)) return Resolve(w);
            return Coerce(w is decimal ? Convert.ToDouble(w, CultureInfo.InvariantCulture) : w, t);
        }

        static object Coerce(object o, Type t)
        {
            if (o == null || t.IsInstanceOfType(o)) return o;
            if (t.IsEnum) return Enum.ToObject(t, o);
            if (IsNumeric(t) || t == typeof(bool) || t == typeof(string)) return Convert.ChangeType(o, t, CultureInfo.InvariantCulture);
            throw new BridgeException("cannot convert " + o.GetType().FullName + " to " + t.FullName);
        }

        // ---- type resolution -------------------------------------------------
        static Type ResolveTypeOrThrow(string name)
        {
            Type t = ResolveType(name);
            if (t == null) throw new BridgeException("type not found: " + name);
            return t;
        }

        static Type ResolveType(string name)
        {
            lock (s_typeCache)
            {
                Type t;
                if (s_typeCache.TryGetValue(name, out t)) return t;
                t = FindType(name);
                if (t == null && name.StartsWith("Siemens.", StringComparison.Ordinal))
                {
                    LoadAllOpenness();
                    t = FindType(name);
                }
                if (t != null) s_typeCache[name] = t;
                return t;
            }
        }

        static Type FindType(string name)
        {
            Type t = Type.GetType(name, false);
            if (t != null) return t;
            foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies())
            {
                try { t = a.GetType(name, false); } catch { t = null; }
                if (t != null) return t;
            }
            return null;
        }

        // ---- member access ---------------------------------------------------
        const BindingFlags Inst = BindingFlags.Public | BindingFlags.Instance;
        const BindingFlags Stat = BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy;

        static IEnumerable<Type> SelfAndInterfaces(Type t)
        {
            yield return t;
            foreach (Type i in t.GetInterfaces()) yield return i;
        }

        static PropertyInfo FindProperty(Type t, string name)
        {
            foreach (Type x in SelfAndInterfaces(t))
            {
                PropertyInfo[] ps;
                try { ps = x.GetProperties(Inst); } catch { continue; }
                foreach (PropertyInfo p in ps)
                    if (p.Name == name && p.GetIndexParameters().Length == 0) return p;
            }
            return null;
        }

        static MethodInfo FindInterfaceMethod(Type t, string iface, string name, int argc)
        {
            foreach (Type i in t.GetInterfaces())
            {
                if (i.Name != iface) continue;
                foreach (MethodInfo m in i.GetMethods())
                    if (m.Name == name && m.GetParameters().Length == argc) return m;
            }
            return null;
        }

        static object GetMember(object target, string name)
        {
            Type t = target.GetType();
            PropertyInfo p = FindProperty(t, name);
            if (p != null && p.CanRead)
            {
                try { return p.GetValue(target, null); }
                catch (TargetInvocationException ex) { throw Unwrap(ex); }
            }
            FieldInfo f = t.GetField(name, Inst);
            if (f != null) return f.GetValue(target);
            MethodInfo ga = FindInterfaceMethod(t, "IEngineeringObject", "GetAttribute", 1);
            if (ga != null)
            {
                try { return ga.Invoke(target, new object[] { name }); }
                catch (TargetInvocationException ex) { throw Unwrap(ex); }
            }
            throw new BridgeException("member '" + name + "' not found on " + t.FullName);
        }

        static void SetMember(object target, string name, object w)
        {
            Type t = target.GetType();
            PropertyInfo p = FindProperty(t, name);
            if (p != null && p.CanWrite)
            {
                try { p.SetValue(target, Convert2(w, p.PropertyType), null); return; }
                catch (TargetInvocationException ex) { throw Unwrap(ex); }
            }
            FieldInfo f = t.GetField(name, Inst);
            if (f != null) { f.SetValue(target, Convert2(w, f.FieldType)); return; }
            MethodInfo sa = FindInterfaceMethod(t, "IEngineeringObject", "SetAttribute", 2);
            if (sa != null)
            {
                object value = Resolve(w);
                if (value != null && !(w is Dictionary<string, object>))
                {
                    // Coerce to the attribute's current runtime type (e.g. enum names, int widths).
                    MethodInfo ga = FindInterfaceMethod(t, "IEngineeringObject", "GetAttribute", 1);
                    object cur = null;
                    try { cur = ga.Invoke(target, new object[] { name }); } catch { cur = null; }
                    if (cur != null && !cur.GetType().IsInstanceOfType(value))
                    {
                        try { value = Convert2(w, cur.GetType()); } catch { }
                    }
                }
                try { sa.Invoke(target, new object[] { name, value }); return; }
                catch (TargetInvocationException ex) { throw Unwrap(ex); }
            }
            throw new BridgeException("writable member '" + name + "' not found on " + t.FullName);
        }

        static bool SigMatches(ParameterInfo[] ps, List<string> sig)
        {
            if (sig == null) return true;
            if (sig.Count != ps.Length) return false;
            for (int i = 0; i < ps.Length; i++)
            {
                Type pt = ps[i].ParameterType;
                if (sig[i] != pt.FullName && sig[i] != pt.Name) return false;
            }
            return true;
        }

        // outs: trailing out parameters may be omitted (and null is accepted for any out parameter).
        static MethodBase Pick(IEnumerable<MethodBase> candidates, object[] args, Type[] generic, List<string> sig, bool outs,
                               out object[] converted)
        {
            MethodBase best = null;
            int bestScore = int.MinValue;
            foreach (MethodBase c in candidates)
            {
                MethodBase m = c;
                var mi = m as MethodInfo;
                if (mi != null && mi.IsGenericMethodDefinition)
                {
                    if (generic == null || mi.GetGenericArguments().Length != generic.Length) continue;
                    try { m = mi.MakeGenericMethod(generic); } catch { continue; }
                }
                else if (generic != null) continue;

                ParameterInfo[] ps;
                try { ps = m.GetParameters(); } catch { continue; } // signature uses an unresolvable internal type
                if (!SigMatches(ps, sig)) continue;
                int required = ps.Count(p => !p.IsOptional && !(outs && p.IsOut));
                if (args.Length < required || args.Length > ps.Length) continue;
                int score = 0;
                bool ok = true;
                for (int i = 0; i < args.Length; i++)
                {
                    int s;
                    if (outs && ps[i].IsOut && args[i] == null) s = 2;
                    else try { s = Score(args[i], ps[i].ParameterType); } catch { s = -1; }
                    if (s < 0) { ok = false; break; }
                    score += s;
                }
                if (!ok) continue;
                score = score * 4 - (ps.Length - args.Length);
                if (score > bestScore) { best = m; bestScore = score; }
            }
            converted = null;
            if (best == null) return null;
            ParameterInfo[] bp = best.GetParameters();
            converted = new object[bp.Length];
            for (int i = 0; i < bp.Length; i++)
                converted[i] = i < args.Length ? Convert2(args[i], bp[i].ParameterType) : bp[i].IsOut ? null : bp[i].DefaultValue;
            return best;
        }

        static string DescribeCandidates(IEnumerable<MethodBase> cs)
        {
            var sb = new StringBuilder();
            foreach (MethodBase m in cs)
            {
                sb.Append("\n  ").Append(m.Name).Append('(');
                try { sb.Append(string.Join(", ", m.GetParameters().Select(p => p.ParameterType.Name + " " + p.Name))); }
                catch { sb.Append("?"); }
                sb.Append(')');
            }
            return sb.ToString();
        }

        static object CallMethod(object target, Type staticType, string name, object[] args, Type[] generic, List<string> sig,
                                 bool outs, out MethodBase picked, out object[] conv)
        {
            picked = null;
            conv = null;
            List<MethodBase> cands = new List<MethodBase>();
            if (target != null)
            {
                foreach (Type x in SelfAndInterfaces(target.GetType()))
                {
                    MethodInfo[] ms;
                    try { ms = x.GetMethods(Inst); } catch { continue; }
                    foreach (MethodInfo m in ms) if (m.Name == name) cands.Add(m);
                }
                if (cands.Count == 0 && args.Length == 0 && generic == null)
                    return GetMember(target, name);
            }
            else
            {
                foreach (MethodInfo m in staticType.GetMethods(Stat)) if (m.Name == name) cands.Add(m);
                if (cands.Count == 0 && args.Length == 0)
                {
                    PropertyInfo p = staticType.GetProperty(name, Stat);
                    if (p != null) return p.GetValue(null, null);
                    FieldInfo f = staticType.GetField(name, Stat);
                    if (f != null) return f.GetValue(null);
                }
            }
            if (cands.Count == 0)
                throw new BridgeException("method '" + name + "' not found on " + (target != null ? target.GetType().FullName : staticType.FullName));
            MethodBase pick = Pick(cands, args, generic, sig, outs, out conv);
            if (pick == null)
                throw new BridgeException("no overload of '" + name + "' matches the " + args.Length + " given argument(s); candidates:" + DescribeCandidates(cands));
            picked = pick;
            try { return pick.Invoke(target, conv); }
            catch (TargetInvocationException ex) { throw Unwrap(ex); }
        }

        static object Construct(Type t, object[] args, List<string> sig)
        {
            var cands = t.GetConstructors().Cast<MethodBase>().ToList();
            if (cands.Count == 0 && args.Length == 0 && t.IsValueType) return Activator.CreateInstance(t);
            object[] conv;
            MethodBase pick = Pick(cands, args, null, sig, false, out conv);
            if (pick == null) throw new BridgeException("no constructor of " + t.FullName + " matches; candidates:" + DescribeCandidates(cands));
            try { return ((ConstructorInfo)pick).Invoke(conv); }
            catch (TargetInvocationException ex) { throw Unwrap(ex); }
        }

        static object GetService(object target, Type serviceType)
        {
            foreach (Type x in SelfAndInterfaces(target.GetType()))
            {
                foreach (MethodInfo m in x.GetMethods(Inst))
                {
                    if (m.Name != "GetService" || !m.IsGenericMethodDefinition || m.GetParameters().Length != 0) continue;
                    try { return m.MakeGenericMethod(serviceType).Invoke(target, null); }
                    catch (TargetInvocationException ex) { throw Unwrap(ex); }
                }
            }
            throw new BridgeException(target.GetType().FullName + " is not a service provider");
        }

        static object IsInstance(object hv, string typeName)
        {
            object o = hv == null ? null : Lookup(LongOf(hv));
            if (o == null) return false;
            Type t = ResolveType(typeName);
            if (t != null) return t.IsInstanceOfType(o);
            // Unknown type name: compare by simple or full name along the hierarchy.
            for (Type x = o.GetType(); x != null; x = x.BaseType)
                if (x.FullName == typeName || x.Name == typeName) return true;
            return o.GetType().GetInterfaces().Any(i => i.FullName == typeName || i.Name == typeName);
        }

        static object ReadOne(object item, string attr)
        {
            try { return ToWire(GetMember(item, attr)); }
            catch (Exception ex)
            {
                var e = new Dictionary<string, object>();
                e["$err"] = Unwrap(ex).Message;
                return e;
            }
        }

        static object ReadAttrs(object target, List<string> attrs)
        {
            var d = new Dictionary<string, object>();
            foreach (string a in attrs) d[a] = ReadOne(target, a);
            return d;
        }

        static object Enumerate(object target, List<string> attrs, int limit)
        {
            var e = target as IEnumerable;
            if (e == null) throw new BridgeException(target.GetType().FullName + " is not enumerable");
            var list = new List<object>();
            foreach (object item in e)
            {
                if (limit >= 0 && list.Count >= limit) break;
                object w = ToWire(item);
                var wd = w as Dictionary<string, object>;
                if (attrs.Count > 0 && wd != null && wd.ContainsKey("$h"))
                    wd["a"] = ReadAttrs(item, attrs);
                list.Add(w);
            }
            return list;
        }

        static object DescribeType(object o)
        {
            var d = new Dictionary<string, object>();
            Type t = o.GetType();
            d["type"] = t.FullName;
            var bases = new List<object>();
            for (Type b = t.BaseType; b != null && b != typeof(object); b = b.BaseType) bases.Add(b.FullName);
            d["bases"] = bases;
            d["interfaces"] = t.GetInterfaces().Select(i => (object)i.FullName).ToList();
            return d;
        }

        // Member listing is diagnostic: a signature that references an internal
        // (unresolvable) TIA assembly is reported as such instead of failing.
        static void TryAdd(List<object> list, Func<string> describe)
        {
            try { list.Add(describe()); }
            catch (Exception ex) { list.Add("<unavailable: " + Unwrap(ex).GetType().Name + ">"); }
        }

        static T[] Safe<T>(Func<T[]> get)
        {
            try { return get(); } catch { return new T[0]; }
        }

        static string MethodText(MethodInfo m, string prefix)
        {
            return prefix + m.ReturnType.Name + " " + m.Name +
                (m.IsGenericMethodDefinition ? "<" + string.Join(",", m.GetGenericArguments().Select(g => g.Name)) + ">" : "") +
                "(" + string.Join(", ", m.GetParameters().Select(p => p.ParameterType.Name + " " + p.Name)) + ")";
        }

        static object ListMembers(Type t)
        {
            var props = new List<object>();
            var methods = new List<object>();
            var events = new List<object>();
            foreach (Type x in SelfAndInterfaces(t))
            {
                Type cur = x;
                foreach (PropertyInfo p in Safe(() => cur.GetProperties(Inst)))
                {
                    PropertyInfo pp = p;
                    TryAdd(props, () => pp.PropertyType.Name + " " + pp.Name + (pp.CanWrite ? " {get;set;}" : " {get;}"));
                }
                foreach (MethodInfo m in Safe(() => cur.GetMethods(Inst)))
                {
                    if (m.IsSpecialName) continue;
                    MethodInfo mm = m;
                    TryAdd(methods, () => MethodText(mm, ""));
                }
                foreach (EventInfo ev in Safe(() => cur.GetEvents(Inst)))
                {
                    EventInfo e = ev;
                    TryAdd(events, () => e.EventHandlerType.Name + " " + e.Name);
                }
            }
            foreach (MethodInfo m in Safe(() => t.GetMethods(Stat)))
            {
                if (m.IsSpecialName) continue;
                MethodInfo mm = m;
                TryAdd(methods, () => MethodText(mm, "static "));
            }
            var d = new Dictionary<string, object>();
            d["type"] = t.FullName;
            d["properties"] = props.Distinct().ToList();
            d["methods"] = methods.Distinct().ToList();
            d["events"] = events.Distinct().ToList();
            if (t.IsEnum) d["enum"] = Enum.GetNames(t).Cast<object>().ToList();
            return d;
        }

        // ---- callbacks: events and delegates ---------------------------------
        static Delegate MakeDelegate(Type delType, long cbId)
        {
            if (!typeof(Delegate).IsAssignableFrom(delType)) throw new BridgeException(delType.FullName + " is not a delegate type");
            MethodInfo inv = delType.GetMethod("Invoke");
            ParameterInfo[] ps = inv.GetParameters();
            ParameterExpression[] pe = ps.Select(p => Expression.Parameter(p.ParameterType, p.Name)).ToArray();
            Expression arr = Expression.NewArrayInit(typeof(object),
                pe.Select(p => (Expression)Expression.Convert(p, typeof(object))));
            MethodInfo disp = typeof(Entry).GetMethod("DispatchCallback", BindingFlags.Public | BindingFlags.Static);
            Expression call = Expression.Call(disp, Expression.Constant(cbId), arr,
                Expression.Constant(inv.ReturnType, typeof(Type)));
            Expression body = inv.ReturnType == typeof(void) ? call : (Expression)Expression.Convert(call, inv.ReturnType);
            return Expression.Lambda(delType, body, pe).Compile();
        }

        public static object DispatchCallback(long id, object[] args, Type ret)
        {
            NativeCallbackFn cb = s_native;
            object result = null;
            if (cb != null)
            {
                ScopeBegin();
                try
                {
                    string json = Json.Serialize(args.Select(ToWire).ToList());
                    IntPtr p = WriteUtf8(json);
                    IntPtr res = IntPtr.Zero;
                    try { cb(id, p, out res); }
                    finally { Marshal.FreeCoTaskMem(p); }
                    if (res != IntPtr.Zero)
                    {
                        string rs = ReadUtf8(res);
                        Marshal.FreeCoTaskMem(res);
                        if (ret != typeof(void) && rs.Length > 0)
                            result = Convert2(Json.DeserializeObject(rs), ret);
                    }
                }
                catch
                {
                    // Never let a native failure propagate into the event source.
                }
                finally
                {
                    ScopeEnd();
                }
            }
            if (result == null && ret != typeof(void) && ret.IsValueType) result = Activator.CreateInstance(ret);
            return result;
        }

        static object Subscribe(object target, string eventName, long cbId)
        {
            EventInfo ev = null;
            foreach (Type x in SelfAndInterfaces(target.GetType()))
            {
                ev = x.GetEvent(eventName, Inst);
                if (ev != null) break;
            }
            if (ev == null) throw new BridgeException("event '" + eventName + "' not found on " + target.GetType().FullName);
            Delegate d = MakeDelegate(ev.EventHandlerType, cbId);
            try { ev.AddEventHandler(target, d); }
            catch (TargetInvocationException ex) { throw Unwrap(ex); }
            lock (s_lock)
            {
                long sub = s_nextSub++;
                s_subs[sub] = new KeyValuePair<object, Delegate>(target, d);
                s_subEvents[sub] = ev;
                return sub;
            }
        }

        static void Unsubscribe(long sub)
        {
            KeyValuePair<object, Delegate> kv;
            EventInfo ev;
            lock (s_lock)
            {
                if (!s_subs.TryGetValue(sub, out kv)) return;
                ev = s_subEvents[sub];
                s_subs.Remove(sub);
                s_subEvents.Remove(sub);
            }
            try { ev.RemoveEventHandler(kv.Key, kv.Value); }
            catch (TargetInvocationException ex) { throw Unwrap(ex); }
        }
    }
}
