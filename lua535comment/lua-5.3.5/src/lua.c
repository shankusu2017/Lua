/*
** $Id: lua.c,v 1.230.1.1 2017/04/19 17:29:57 roberto Exp $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*/

#define lua_c

#include "lprefix.h"


#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"



#if !defined(LUA_PROMPT)
#define LUA_PROMPT		"> "
#define LUA_PROMPT2		">> "
#endif

#if !defined(LUA_PROGNAME)
#define LUA_PROGNAME		"lua"
#endif

#if !defined(LUA_MAXINPUT)
#define LUA_MAXINPUT		512
#endif

#if !defined(LUA_INIT_VAR)
#define LUA_INIT_VAR		"LUA_INIT"
#endif

#define LUA_INITVARVERSION	LUA_INIT_VAR LUA_VERSUFFIX


/*
** lua_stdin_is_tty detects whether the standard input is a 'tty' (that
** is, whether we're running lua interactively).
*/
#if !defined(lua_stdin_is_tty)	/* { */

#if defined(LUA_USE_POSIX)	/* { */

#include <unistd.h>
#define lua_stdin_is_tty()	isatty(0)

#elif defined(LUA_USE_WINDOWS)	/* }{ */

#include <io.h>
#include <windows.h>

#define lua_stdin_is_tty()	_isatty(_fileno(stdin))

#else				/* }{ */

/* ISO C definition */
#define lua_stdin_is_tty()	1  /* assume stdin is a tty */

#endif				/* } */

#endif				/* } */


/*
** lua_readline defines how to show a prompt and then read a line from
** the standard input.
** lua_saveline defines how to "save" a read line in a "history".
** lua_freeline defines how to free a line read by lua_readline.
*/
#if !defined(lua_readline)	/* { */

#if defined(LUA_USE_READLINE)	/* { */

#include <readline/readline.h>
#include <readline/history.h>
#define lua_readline(L,b,p)	((void)L, ((b)=readline(p)) != NULL)
#define lua_saveline(L,line)	((void)L, add_history(line))
#define lua_freeline(L,b)	((void)L, free(b))

#else				/* }{ */

#define lua_readline(L,b,p) \
        ((void)L, fputs(p, stdout), fflush(stdout),  /* show prompt */ \
        fgets(b, LUA_MAXINPUT, stdin) != NULL)  /* get line */
#define lua_saveline(L,line)	{ (void)L; (void)line; }
#define lua_freeline(L,b)	{ (void)L; (void)b; }

#endif				/* } */

#endif				/* } */




static lua_State *globalL = NULL;

static const char *progname = LUA_PROGNAME;


/*
** Hook set by signal function to stop the interpreter.
*/
static void lstop (lua_State *L, lua_Debug *ar) {
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);  /* reset hook */
  luaL_error(L, "interrupted!");
}


/*
** Function to be called at a C signal. Because a C signal cannot
** just change a Lua state (as there is no proper synchronization),
** this function only sets a hook that, when called, will stop the
** interpreter.
*/
static void laction (int i) {
  signal(i, SIG_DFL); /* if another SIGINT happens, terminate process */
  lua_sethook(globalL, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}


static void print_usage (const char *badoption) {
  lua_writestringerror("%s: ", progname);
  if (badoption[1] == 'e' || badoption[1] == 'l')
    lua_writestringerror("'%s' needs argument\n", badoption);
  else
    lua_writestringerror("unrecognized option '%s'\n", badoption);
  lua_writestringerror(
  "usage: %s [options] [script [args]]\n"
  "Available options are:\n"
  "  -e stat  execute string 'stat'\n"
  "  -i       enter interactive mode after executing 'script'\n"
  "  -l name  require library 'name' into global 'name'\n"
  "  -v       show version information\n"
  "  -E       ignore environment variables\n"
  "  --       stop handling options\n"
  "  -        stop handling options and execute stdin\n"
  ,
  progname);
}


/*
** Prints an error message, adding the program name in front of it
** (if present)
*/
static void l_message (const char *pname, const char *msg) {
  if (pname) lua_writestringerror("%s: ", pname);
  lua_writestringerror("%s\n", msg);
}


/*
** Check whether 'status' is not OK and, if so, prints the error
** message on the top of the stack. It assumes that the error object
** is a string, as it was either generated by Lua or by 'msghandler'.
*/
static int report (lua_State *L, int status) {
  if (status != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    l_message(progname, msg);
    lua_pop(L, 1);  /* remove message */
  }
  return status;
}


/*
** Message handler used to run all chunks
*/
static int msghandler (lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == NULL) {  /* is error object not a string? */
    if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
        lua_type(L, -1) == LUA_TSTRING)  /* that produces a string? */
      return 1;  /* that is the message */
    else
      msg = lua_pushfstring(L, "(error object is a %s value)",
                               luaL_typename(L, 1));
  }
  luaL_traceback(L, L, msg, 1);  /* append a standard traceback */
  return 1;  /* return the traceback */
}


/*
** Interface to 'lua_pcall', which sets appropriate message function
** and C-signal handler. Used to run all chunks.
*/
/*
** narg是即将被执行函数的参数个数，参数紧随在函数closure对象后面。在调用该函数值之前
** 需要将参数在栈中设置好。没有参数的话，就不用设置。
*/
static int docall (lua_State *L, int narg, int nres) {
  int status;

  /* 获取即将被执行的函数在栈中的索引 */
  int base = lua_gettop(L) - narg;  /* function index */

  /* 往栈顶部压入消息处理函数对应的CClosure对象。 */
  lua_pushcfunction(L, msghandler);  /* push message handler */
  /*
  ** 将消息处理函数对应的CClosure对象从栈顶往下移至被调用函数对应的closure对象下面，
  ** 此时，消息处理函数对应的CClosure对象上面是即将被调用的函数的closure对象，往上至
  ** 栈顶部都是该函数的参数。
  */
  lua_insert(L, base);  /* put it under function and args */
  
  globalL = L;  /* to be available to 'laction' */
  signal(SIGINT, laction);  /* set C-signal handler */

  /* 由于上面执行的insert操作，使得索引base对应的栈单元中存放的是消息处理函数对应的CClosure对象 */
  status = lua_pcall(L, narg, nres, base);

  signal(SIGINT, SIG_DFL); /* reset C-signal handler */
  lua_remove(L, base);  /* remove message handler from the stack */
  return status;
}


static void print_version (void) {
  lua_writestring(LUA_COPYRIGHT, strlen(LUA_COPYRIGHT));
  lua_writeline();
}


/*
** Create the 'arg' table, which stores all arguments from the
** command line ('argv'). It should be aligned so that, at index 0,
** it has 'argv[script]', which is the script name. The arguments
** to the script (everything after 'script') go to positive indices;
** other arguments (before the script name) go to negative indices.
** If there is no script name, assume interpreter's name as base.
*/
/*
** createargtable()用于创建一个arg表，包含了所有的命令行参数，索引为数字。
** createargtable()函数要创建的arg table具有如下规则：
** 命令行参数中lua代码的文件在这个表的的索引为0，命令行参数中在lua代码文件左边的参数
** 在arg table中的索引是负的，而且从左到右依次递增；命令行参数中在lua代码文件右边的
** 参数在args table中的索引是正的，而且从左到右依次递增。
** 举例如下：命令行参数为：lua -e "print('helloworld')" test.lua a b
** 则在arg表中有如下内容：
** arg[-3] = "lua" arg[-2] = "-e" arg[-1] = "print('helloworld')"
** arg[0] = "test.lua" arg[1] = "a" arg[2] = "b"
*/
static void createargtable (lua_State *L, char **argv, int argc, int script) {
  int i, narg;
  /*
  ** script这个参数的内容就是collectargs()函数中传递出来的，表示第一个没有被处理的参数在argv中的索引。
  ** 通过分析collectargs()函数的代码，我们知道，如果命令行参数中没有指定lua代码的脚本文件，那么first
  ** 存放的值和命令行参数的个数是相等的，即等于pmain()中的argc。
  */
  if (script == argc) script = 0;  /* no script name? */
  /*
  ** 计算在args表中索引值为正的参数个数，即在lua代码文件右边的命令行参数个数
  ** 注意，pmain()中的命令行参数个数argc是包括lua可执行文件的。
  */
  narg = argc - (script + 1);  /* number of positive indices */

  /*
  ** 创建一个lua表（arg)，注意到，其数组部分的大小为narg，即上面的正索引值的参数个数，hash表部分的
  ** 大小为script + 1，即在lua代码文件左边以及lua代码文件自身的参数个数。从这里我们可以知道，索引值为
  ** 正的那部分命令行参数是存放到table的数组部分的，索引值为0和负的那部分命令行参数是存放到table的hash
  ** 表部分的。
  ** 下面这条创建lua表的语句，在创建完table之后，会将其压入栈顶部。
  */
  lua_createtable(L, narg, script + 1);

  /* 将所有的参数压入到args表中，下面的-2即为args表在栈中的索引值(对应L->top - 2) */
  for (i = 0; i < argc; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - script);
  }

  /*
  ** 将上面创建的table以"arg"为键值添加到全局表_G中，同时上面创建的arg表也会从栈顶部弹出。
  ** 由于这里将arg表添加到了_G中，因此我们在lua代码中可以直接访问arg表。
  */
  lua_setglobal(L, "arg");
}


/*
** 执行代码块，status表示的是对待执行代码块词法分析和语法分析的结果，如果结果OK，那么就
** 可以开始执行代码块了。
*/
static int dochunk (lua_State *L, int status) {
  if (status == LUA_OK) status = docall(L, 0, 0);
  return report(L, status);
}

/* 加载并执行name指定的lua代码文件 */
static int dofile (lua_State *L, const char *name) {
  return dochunk(L, luaL_loadfile(L, name));
}

/* 字符串s中包含了可执行的lua代码。例如以-e加一个包含了lua代码的字符串来启动lua，就会走这个流程。 */
static int dostring (lua_State *L, const char *s, const char *name) {
  return dochunk(L, luaL_loadbuffer(L, s, strlen(s), name));
}


/*
** Calls 'require(name)' and stores the result in a global variable
** with the given name.
*/
/*
** dolibrary()函数的功能就是执行reqire 'name'。然后将require函数的返回值（比如库级别的table）以
** name为键值加入到_G表中。
*/
static int dolibrary (lua_State *L, const char *name) {
  int status;
  /* 往栈顶部压入require对应的CClosure对象 */
  lua_getglobal(L, "require");

  /* 将库名name压入栈顶部 */
  lua_pushstring(L, name);

  /* 执行require函数调用，结果会在require函数内压入栈顶部。require对应的函数是ll_require() */
  status = docall(L, 1, 1);  /* call 'require(name)' */
  if (status == LUA_OK)
    lua_setglobal(L, name);  /* global[name] = require return */
  return report(L, status);
}


/*
** Returns the string to be used as a prompt by the interpreter.
*/
static const char *get_prompt (lua_State *L, int firstline) {
  const char *p;
  lua_getglobal(L, firstline ? "_PROMPT" : "_PROMPT2");
  p = lua_tostring(L, -1);
  if (p == NULL) p = (firstline ? LUA_PROMPT : LUA_PROMPT2);
  return p;
}

/* mark in error messages for incomplete statements */
#define EOFMARK		"<eof>"
#define marklen		(sizeof(EOFMARK)/sizeof(char) - 1)


/*
** Check whether 'status' signals a syntax error and the error
** message at the top of the stack ends with the above mark for
** incomplete statements.
*/
static int incomplete (lua_State *L, int status) {
  if (status == LUA_ERRSYNTAX) {
    size_t lmsg;
    const char *msg = lua_tolstring(L, -1, &lmsg);
    if (lmsg >= marklen && strcmp(msg + lmsg - marklen, EOFMARK) == 0) {
      lua_pop(L, 1);
      return 1;
    }
  }
  return 0;  /* else... */
}


/*
** Prompt the user, read a line, and push it into the Lua stack.
*/
static int pushline (lua_State *L, int firstline) {
  char buffer[LUA_MAXINPUT];
  char *b = buffer;
  size_t l;
  const char *prmt = get_prompt(L, firstline);
  int readstatus = lua_readline(L, b, prmt);
  if (readstatus == 0)
    return 0;  /* no input (prompt will be popped by caller) */
  lua_pop(L, 1);  /* remove prompt */
  l = strlen(b);
  if (l > 0 && b[l-1] == '\n')  /* line ends with newline? */
    b[--l] = '\0';  /* remove it */
  if (firstline && b[0] == '=')  /* for compatibility with 5.2, ... */
    lua_pushfstring(L, "return %s", b + 1);  /* change '=' to 'return' */
  else
    lua_pushlstring(L, b, l);
  lua_freeline(L, b);
  return 1;
}


/*
** Try to compile line on the stack as 'return <line>;'; on return, stack
** has either compiled chunk or original line (if compilation failed).
*/
static int addreturn (lua_State *L) {
  const char *line = lua_tostring(L, -1);  /* original line */
  const char *retline = lua_pushfstring(L, "return %s;", line);
  int status = luaL_loadbuffer(L, retline, strlen(retline), "=stdin");
  if (status == LUA_OK) {
    lua_remove(L, -2);  /* remove modified line */
    if (line[0] != '\0')  /* non empty? */
      lua_saveline(L, line);  /* keep history */
  }
  else
    lua_pop(L, 2);  /* pop result from 'luaL_loadbuffer' and modified line */
  return status;
}


/*
** Read multiple lines until a complete Lua statement
*/
static int multiline (lua_State *L) {
  for (;;) {  /* repeat until gets a complete statement */
    size_t len;
    const char *line = lua_tolstring(L, 1, &len);  /* get what it has */
    int status = luaL_loadbuffer(L, line, len, "=stdin");  /* try it */
    if (!incomplete(L, status) || !pushline(L, 0)) {
      lua_saveline(L, line);  /* keep history */
      return status;  /* cannot or should not try to add continuation line */
    }
    lua_pushliteral(L, "\n");  /* add newline... */
    lua_insert(L, -2);  /* ...between the two lines */
    lua_concat(L, 3);  /* join them */
  }
}


/*
** Read a line and try to load (compile) it first as an expression (by
** adding "return " in front of it) and second as a statement. Return
** the final status of load/call with the resulting function (if any)
** in the top of the stack.
*/
static int loadline (lua_State *L) {
  int status;
  lua_settop(L, 0);
  if (!pushline(L, 1))
    return -1;  /* no input */
  if ((status = addreturn(L)) != LUA_OK)  /* 'return ...' did not work? */
    status = multiline(L);  /* try as command, maybe with continuation lines */
  lua_remove(L, 1);  /* remove line from the stack */
  lua_assert(lua_gettop(L) == 1);
  return status;
}


/*
** Prints (calling the Lua 'print' function) any values on the stack
*/
static void l_print (lua_State *L) {
  int n = lua_gettop(L);
  if (n > 0) {  /* any result to be printed? */
    luaL_checkstack(L, LUA_MINSTACK, "too many results to print");
    lua_getglobal(L, "print");
    lua_insert(L, 1);
    if (lua_pcall(L, n, 0, 0) != LUA_OK)
      l_message(progname, lua_pushfstring(L, "error calling 'print' (%s)",
                                             lua_tostring(L, -1)));
  }
}


/*
** Do the REPL: repeatedly read (load) a line, evaluate (call) it, and
** print any results.
*/
static void doREPL (lua_State *L) {
  int status;
  const char *oldprogname = progname;
  progname = NULL;  /* no 'progname' on errors in interactive mode */
  while ((status = loadline(L)) != -1) {
    if (status == LUA_OK)
      status = docall(L, 0, LUA_MULTRET);
    if (status == LUA_OK) l_print(L);
    else report(L, status);
  }
  lua_settop(L, 0);  /* clear stack */
  lua_writeline();
  progname = oldprogname;
}


/*
** Push on the stack the contents of table 'arg' from 1 to #arg
*/
static int pushargs (lua_State *L) {
  int i, n;
  if (lua_getglobal(L, "arg") != LUA_TTABLE)
    luaL_error(L, "'arg' is not a table");
  n = (int)luaL_len(L, -1);
  luaL_checkstack(L, n + 3, "too many arguments to script");
  for (i = 1; i <= n; i++)
    lua_rawgeti(L, -i, i);
  lua_remove(L, -i);  /* remove table from the stack */
  return n;
}


/* 
** 执行命令行参数中指定的lua代码文件，从handle_script()函数调用的地方可以看出，
** argv[0]就是lua代码文件名对应的字符串，
*/
static int handle_script (lua_State *L, char **argv) {
  int status;
  const char *fname = argv[0];
  if (strcmp(fname, "-") == 0 && strcmp(argv[-1], "--") != 0)
    fname = NULL;  /* stdin */

  /* 
  ** 在执行luaL_loadfile()之前，虚拟栈中目前的元素有pmain对应的CClosure对象，argc和
  ** argv这三个元素。
  */
  
  /*
  ** 加载lua代码文件，但未执行，因为lua代码文件可能需要命令行参数。加载完之后生成的LClosure对象
  ** 会被压入栈顶部。
  */
  status = luaL_loadfile(L, fname);

  /*
  ** 程序执行到这里时，位于栈顶部的是fname代码文件解析后生成的LClosure对象。注意，
  ** 一个代码文件，字符串中包含了lua代码，交互模式中一行的代码最终都会被解析成一个
  ** LClosure对象，解析完之后会被压入栈顶部。
  */

  /*
  ** 如果加载lua代码文件成功，则尝试从arg表中将lua代码文件的参数压入栈中，供执行文件的时候使用。
  ** 在构造arg表的时候，lua代码文件在arg表中的索引为0，而在lua代码文件右边的命令行参数的索引值
  ** 均为正，且存放在arg表的数组部分，因此我们只需要将数组部分的元素依序压入栈中即可，同时返回
  ** 数组部分的元素个数。注意有时候在lua代码文件右边的命令行参数可能还会是一些选项，类似下面：
  ** lua -e "print(math.sin(2)" test.lua a b -v
  ** 对于上面这样的情况，我们也会将"-v"压入栈中，但不影响函数的执行结果。
  */
  if (status == LUA_OK) {
    int n = pushargs(L);  /* push arguments to script */
    status = docall(L, n, LUA_MULTRET);
  }
  return report(L, status);
}



/* bits of various argument indicators in 'args' */
#define has_error	1	/* bad option */
#define has_i		2	/* -i */
#define has_v		4	/* -v */
#define has_e		8	/* -e */
#define has_E		16	/* -E */

/*
** Traverses all arguments from 'argv', returning a mask with those
** needed before running any Lua code (or an error code if it finds
** any invalid argument). 'first' returns the first not-handled argument
** (either the script name or a bad argument in case of error).
*/
/*
** Lua的命令行格式如下：
** lua [options] [script [args]]
** options是列出的选项，script则是lua代码文件，args是传递给lua代码文件的参数
** 
** Lua的命令行选项如下：
** -e 直接将待执行的lua代码用双引号括起来，跟在-e后面，可以有空格，也可以没空格，
** 	  如：lua -e "print(math.sin(2))"
** -i 以交互式的方式启动lua。
** -l 加载并执行一个库文件，文件名字跟在-l后面，可以有空格，也可以没空格直接跟在-l后面。
** -v 显示版本号
** -E 忽略环境变量
** -- 停止处理后面的选项
** -  停止处理后面的选项，并执行stdin中的内容
** first参数返回的是第一个没有处理的参数在argv中的索引，通过下面的代码分析我们知道，如果
** 命令行参数中没有lua代码的脚本文件，那么first存放的值和命令行参数的个数是相等的，即等于
** pmain()中的argc。
** 返回值args是选项对应的标记位组合。
*/
static int collectargs (char **argv, int *first) {
  int args = 0;
  int i;
  /* argv[0]就是lua自身，所以从1开始遍历命令行参数数组。 */
  for (i = 1; argv[i] != NULL; i++) {
    *first = i;
    /*
    ** Lua的命令行选项都是以'-'开头的，如果不是，则停止处理并返回。这个地方可能是单独的lua代码文件，
    ** 也有可能是非法参数，也有可能是-e后面跟的包含了lua代码的字符串，也有可能是-l后面跟的库文件。如果
    ** 出现了不以'-'开头的命令行参数，则直接从该函数返回，后面的参数也不再处理了。
    */
    if (argv[i][0] != '-')  /* not an option? */
        return args;  /* stop handling options */
	/* 如果 */
    switch (argv[i][1]) {  /* else check option */
      case '-':  /* '--' */
        if (argv[i][2] != '\0')  /* extra characters after '--'? */
          return has_error;  /* invalid option */
        *first = i + 1;
        return args;
      case '\0':  /* '-' */
        return args;  /* script "name" is '-' */
      case 'E':
        if (argv[i][2] != '\0')  /* extra characters after 1st? */
          return has_error;  /* invalid option */
        args |= has_E;
        break;
      case 'i':
        args |= has_i;  /* (-i implies -v) *//* FALLTHROUGH */
      case 'v':
        if (argv[i][2] != '\0')  /* extra characters after 1st? */
          return has_error;  /* invalid option */
        args |= has_v;
        break;
      case 'e':
        args |= has_e;  /* FALLTHROUGH *//* -e和-l都需要再接参数，所以这里不break，而是继续往下走*/
      case 'l':  /* both options need an argument */
        if (argv[i][2] == '\0') {  /* no concatenated argument? */
          i++;  /* try next 'argv' */
		  /* 
		  ** -e和-l之后还要再接一个参数，如果下一个命令行参数为空或者是另一个选项的开始的话，
		  ** 那么就报错。
		  */
          if (argv[i] == NULL || argv[i][0] == '-')
            return has_error;  /* no next argument or it is another option */
        }
        break;
      default:  /* invalid option */
        return has_error;
    }
  }
  *first = i;  /* no script name */
  return args;
}


/*
** Processes options 'e' and 'l', which involve running Lua code.
** Returns 0 if some code raises an error.
*/
/*
** 从runargs()调用的地方可以知道，参数n就是命令行参数中第一个非选项类的参数，比如可能是
** 单独的lua代码文件，也有可能是-e后面跟的包含了lua代码的字符串，也有可能是-l后面跟的库文件。
*/
static int runargs (lua_State *L, char **argv, int n) {
  int i;
  for (i = 1; i < n; i++) {
    int option = argv[i][1];
    lua_assert(argv[i][0] == '-');  /* already checked */
    /* 判断选项是不是-e或者-l，如果不是的话，则跳过，这里还需要区分的是单独的lua代码文件 */
    if (option == 'e' || option == 'l') {
      int status;
      /* 
      ** 注意：-e和-l选项的参数可以直接跟在它们后面，也可以加空格后跟在后面，所以这里需要
      ** 判断是哪种情况。
      */
      const char *extra = argv[i] + 2;  /* both options need an argument */
      if (*extra == '\0') extra = argv[++i];
	  
      lua_assert(extra != NULL);

	  /* -e和-l分别采取不同的执行方式，-l主要是加载并执行库文件，如果执行失败，则返回0 */
      status = (option == 'e')
               ? dostring(L, extra, "=(command line)")
               : dolibrary(L, extra);
      if (status != LUA_OK) return 0;
    }
  }
  
  /* 执行成功，返回1。 */
  return 1;
}


/* 
** 处理环境变量"LUA_INIT"
** 如果没有获取到环境变量"LUA_INIT"的内容，比如为空，则直接返回；
** 如果"LUA_INIT"环境变量的内容以"@"开头，则lua会将"@"之后内容当做lua文件名，然后加载并执行该文件。
** 如果"LUA_INIT"环境变量的内容不以"@"开头，那么lua将其当做lua代码，会加载并执行它，
*/
static int handle_luainit (lua_State *L) {

  /* 获取环境变量"LUA_INIT"的内容 */
  const char *name = "=" LUA_INITVARVERSION;
  const char *init = getenv(name + 1);
  if (init == NULL) {
    name = "=" LUA_INIT_VAR;
    init = getenv(name + 1);  /* try alternative name */
  }

  /* 
  ** 如果没有获取到环境变量"LUA_INIT"的内容，比如为空，则直接返回；
  ** 如果"LUA_INIT"环境变量的内容以"@"开头，则lua会将"@"之后内容当做lua文件名，然后加载并执行该文件。
  ** 如果"LUA_INIT"环境变量的内容不以"@"开头，那么lua将其当做lua代码，会加载并执行它，
  */
  if (init == NULL) return LUA_OK;
  else if (init[0] == '@')
    return dofile(L, init+1);
  else
    return dostring(L, init, name);
}


/*
** Main body of stand-alone interpreter (to be called in protected mode).
** Reads the options and handles them all.
*/
static int pmain (lua_State *L) {

  /* 获取main函数中传递进来的argc argv */
  int argc = (int)lua_tointeger(L, 1);
  char **argv = (char **)lua_touserdata(L, 2);
  int script;

  /* 
  ** 将一些需要在运行lua代码之前就执行的命令行参数映射成对应的bit，后续只要
  ** 检查相应的bit是否置位，即可判断命令行中是否包含该参数。
  */
  int args = collectargs(argv, &script);
  luaL_checkversion(L);  /* check that interpreter has correct version */
  if (argv[0] && argv[0][0]) progname = argv[0];
  if (args == has_error) {  /* bad arg? */
    print_usage(argv[script]);  /* 'script' has index of bad arg. */
    return 0;
  }

  /* 检查是否有"-v"，有的话，打印版本号信息 */
  if (args & has_v)  /* option '-v'? */
    print_version();

  /*
  ** 检查是否有"-E"，有的话，向全局注册表中写入键值对{"LUA_NOENV": 1}，表示忽略env. vars.。
  ** 添加完之后，将此时位于栈顶部的bool值1弹出栈顶部。
  */
  if (args & has_E) {  /* option '-E'? */
    lua_pushboolean(L, 1);  /* signal for libraries to ignore env. vars. */
    lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
  }

  /* 加载lua中的标准库 */
  luaL_openlibs(L);  /* open standard libraries */

  /* 创建一个arg表，包含了所有的命令行参数，索引为数字。然后添加到全局表_G中。 */
  createargtable(L, argv, argc, script);  /* create table 'arg' */

  /* 没有"-E"参数的时候启动lua，则调用handle_luainit()处理环境变量"LUA_INIT" */
  if (!(args & has_E)) {  /* no option '-E'? */
    if (handle_luainit(L) != LUA_OK)  /* run LUA_INIT */
      return 0;  /* error running LUA_INIT */
  }

  /* 处理-e和-l选项，如果出错，runargs()会返回0 */
  if (!runargs(L, argv, script))  /* execute arguments -e and -l */
    return 0;  /* something failed */

  /*
  ** script == argc说明命令行参数中没有指定lua代码文件，否则就指定了lua代码文件。如果
  ** 指定了lua代码文件，那么就调用handle_script()执行该代码文件，执行完后退出lua解释器。
  */
  if (script < argc &&  /* execute main script (if there is one) */
      handle_script(L, argv + script) != LUA_OK)
    return 0;
  
  /* 如果命令行中指定了-i选项，则以交互模式打开lua解释器。 */
  if (args & has_i)  /* -i option? */
    doREPL(L);  /* do read-eval-print loop */
  else if (script == argc && !(args & (has_e | has_v))) {  /* no arguments? */
    /*
    ** 如果命令行参数中没有指定lua代码文件，也没有指定-e和-v选项，则以交互模式运行lua解释器。
    ** 在交互模式中，用户输入一串代码，按回车，解释器就执行这串代码，否则等待用户输入。
    */
    /* 
    ** 在执行doREPL()或者dofile之前，虚拟栈中目前的元素有pmain对应的CClosure对象，argc和
    ** argv这三个元素。
    */
    if (lua_stdin_is_tty()) {  /* running in interactive mode? */
      print_version();
      doREPL(L);  /* do read-eval-print loop */
    }
    else dofile(L, NULL);  /* executes stdin as a file */
  }

  /* 向栈顶部压入bool值true。然后退出 */
  lua_pushboolean(L, 1);  /* signal no errors */
  return 1;
}


int main (int argc, char **argv) {
  int status, result;
  /* 主要是创建和初始化主线程对应的lua_State状态信息和由全部thread共享的lua_State状态信息 */
  lua_State *L = luaL_newstate();  /* create state */
  if (L == NULL) {
    l_message(argv[0], "cannot create state: not enough memory");
    return EXIT_FAILURE;
  }

  /*
  ** 以保护模式来调用pmain()函数。步骤如下：
  ** 1、将pmain压入虚拟栈。
  ** 2、将argc、argv压入虚拟栈。
  ** 3、执行函数调用。
  ** 4、从虚拟栈中获取函数调用结果。
  */
  lua_pushcfunction(L, &pmain);  /* to call 'pmain' in protected mode */
  lua_pushinteger(L, argc);  /* 1st argument */
  lua_pushlightuserdata(L, argv); /* 2nd argument */

  /* 会用保护模式来执行pmain()函数 */
  status = lua_pcall(L, 2, 1, 0);  /* do the call */
  result = lua_toboolean(L, -1);  /* get result */
  
  report(L, status);
  lua_close(L);
  return (result && status == LUA_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
