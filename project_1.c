#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>            //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <pwd.h>

const char * sysname = "shellgibi";

//char outFile[100];
char *args[51];
//int redirect(char *args[], char outFile[]);

enum return_codes {
    SUCCESS = 0,
    EXIT = 1,
    UNKNOWN = 2,
};
struct command_t {
    char *name;
    bool background;
    bool auto_complete;
    int arg_count;
    char **args;
    char *redirects[3]; // in/out redirection
    struct command_t *next; // for piping
};
/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t * command)
{
    int i=0;
    printf("Command: <%s>\n", command->name);
    printf("\tIs Background: %s\n", command->background?"yes":"no");
    printf("\tNeeds Auto-complete: %s\n", command->auto_complete?"yes":"no");
    printf("\tRedirects:\n");
    for (i=0;i<3;i++)
        printf("\t\t%d: %s\n", i, command->redirects[i]?command->redirects[i]:"N/A");
    printf("\tArguments (%d):\n", command->arg_count);
    for (i=0;i<command->arg_count;++i)
        printf("\t\tArg %d: %s\n", i, command->args[i]);
    if (command->next)
    {
        printf("\tPiped to:\n");
        print_command(command->next);
    }
}
/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
    if (command->arg_count)
    {
        for (int i=0; i<command->arg_count; ++i)
            free(command->args[i]);
        free(command->args);
    }
    for (int i=0;i<3;++i)
        if (command->redirects[i])
            free(command->redirects[i]);
    if (command->next)
    {
        free_command(command->next);
        command->next=NULL;
    }
    free(command->name);
    free(command);
    return 0;
}
/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
    char cwd[1024], hostname[1024];
    gethostname(hostname, sizeof(hostname));
    getcwd(cwd, sizeof(cwd));
    printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
    return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
    const char *splitters=" \t"; // split at whitespace
    int index, len;
    len=strlen(buf);
    while (len>0 && strchr(splitters, buf[0])!=NULL) // trim left whitespace
    {
        buf++;
        len--;
    }
    while (len>0 && strchr(splitters, buf[len-1])!=NULL)
        buf[--len]=0; // trim right whitespace

    if (len>0 && buf[len-1]=='?') // auto-complete
        command->auto_complete=true;
    if (len>0 && buf[len-1]=='&') // background
        command->background=true;

    char *pch = strtok(buf, splitters);
    command->name=(char *)malloc(strlen(pch)+1);
    if (pch==NULL)
        command->name[0]=0;
    else
        strcpy(command->name, pch);

    command->args=(char **)malloc(sizeof(char *));

    int redirect_index;
    int arg_index=0;
    char temp_buf[1024], *arg;
    while (1)
    {
        // tokenize input on splitters
        pch = strtok(NULL, splitters);
        if (!pch) break;
        arg=temp_buf;
        strcpy(arg, pch);
        len=strlen(arg);

        if (len==0) continue; // empty arg, go for next
        while (len>0 && strchr(splitters, arg[0])!=NULL) // trim left whitespace
        {
            arg++;
            len--;
        }
        while (len>0 && strchr(splitters, arg[len-1])!=NULL) arg[--len]=0; // trim right whitespace
        if (len==0) continue; // empty arg, go for next

        // piping to another command
        if (strcmp(arg, "|")==0)
        {
            struct command_t *c=malloc(sizeof(struct command_t));
            int l=strlen(pch);
            pch[l]=splitters[0]; // restore strtok termination
            index=1;
            while (pch[index]==' ' || pch[index]=='\t') index++; // skip whitespaces

            parse_command(pch+index, c);
            pch[l]=0; // put back strtok termination
            command->next=c;
            continue;
        }

        // background process
        if (strcmp(arg, "&")==0)
            continue; // handled before

        // handle input redirection
        redirect_index=-1;
        if (arg[0]=='<')
            redirect_index=0;
        if (arg[0]=='>')
        {
            if (len>1 && arg[1]=='>')
            {
                redirect_index=2;
                arg++;
                len--;
            }
            else redirect_index=1;
        }
        if (redirect_index != -1)
        {
            command->redirects[redirect_index]=malloc(len);
            strcpy(command->redirects[redirect_index], arg+1);
            continue;
        }

        // normal arguments
        if (len>2 && ((arg[0]=='"' && arg[len-1]=='"')
                      || (arg[0]=='\'' && arg[len-1]=='\''))) // quote wrapped arg
        {
            arg[--len]=0;
            arg++;
        }
        command->args=(char **)realloc(command->args, sizeof(char *)*(arg_index+1));
        command->args[arg_index]=(char *)malloc(len+1);
        strcpy(command->args[arg_index++], arg);
    }
    command->arg_count=arg_index;
    return 0;
}
void prompt_backspace()
{
    putchar(8); // go back 1
    putchar(' '); // write empty over
    putchar(8); // go back 1 again
}
/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
    int index=0;
    char c;
    char buf[4096];
    static char oldbuf[4096];

    // tcgetattr gets the parameters of the current terminal
    // STDIN_FILENO will tell tcgetattr that it should write the settings
    // of stdin to oldt
    static struct termios backup_termios, new_termios;
    tcgetattr(STDIN_FILENO, &backup_termios);
    new_termios = backup_termios;
    // ICANON normally takes care that one line at a time will be processed
    // that means it will return if it sees a "\n" or an EOF or an EOL
    new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
    // Those new settings will be set to STDIN
    // TCSANOW tells tcsetattr to change attributes immediately.
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);


    //FIXME: backspace is applied before printing chars
    show_prompt();
    int multicode_state=0;
    buf[0]=0;
    while (1)
    {
        c=getchar();
        // printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

        if (c==9) // handle tab
        {
            buf[index++]='?'; // autocomplete
            break;
        }

        if (c==127) // handle backspace
        {
            if (index>0)
            {
                prompt_backspace();
                index--;
            }
            continue;
        }
        if (c==27 && multicode_state==0) // handle multi-code keys
        {
            multicode_state=1;
            continue;
        }
        if (c==91 && multicode_state==1)
        {
            multicode_state=2;
            continue;
        }
        if (c==65 && multicode_state==2) // up arrow
        {
            int i;
            while (index>0)
            {
                prompt_backspace();
                index--;
            }
            for (i=0;oldbuf[i];++i)
            {
                putchar(oldbuf[i]);
                buf[i]=oldbuf[i];
            }
            index=i;
            continue;
        }
        else
            multicode_state=0;

        putchar(c); // echo the character
        buf[index++]=c;
        if (index>=sizeof(buf)-1) break;
        if (c=='\n') // enter key
            break;
        if (c==4) // Ctrl+D
            return EXIT;
    }
    if (index>0 && buf[index-1]=='\n') // trim newline from the end
        index--;
    buf[index++]=0; // null terminate string

    strcpy(oldbuf, buf);

    parse_command(buf, command);

    // print_command(command); // DEBUG: uncomment for debugging

    // restore the old settings
    tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
    return SUCCESS;
}
int process_command(struct command_t *command);

int main()
{
    while (1)
    {
        struct command_t *command=malloc(sizeof(struct command_t));
        memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

        int code;
        code = prompt(command);
        if (code==EXIT) break;

        code = process_command(command);
        if (code==EXIT) break;

        free_command(command);
    }

    printf("\n");
    return 0;
}

int process_command(struct command_t *command)
{
    int r;
    if (strcmp(command->name, "")==0) return SUCCESS;

    if (strcmp(command->name, "exit")==0)
        return EXIT;

    if (strcmp(command->name, "cd")==0)
    {
        if (command->arg_count > 0)
        {
            r=chdir(command->args[0]);
            if (r==-1)
                printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
            return SUCCESS;
        }
    }
        ///Part III (b) job management
    else if (strcmp(command->name, "myjobs")==0)
    {
        //TODO: DONE
        //We use ps -u "user" command to get all processes
        char *p = getenv("USER");
        char term[128];
        snprintf(term, sizeof(term), "ps -u %s", p);
        system(term);
        return SUCCESS;
    }
    else if (strcmp(command->name, "pause")==0)
    {
        //TODO: DONE
        //we send a stop signal to the given process
        const char *str = command->args[0];
        int x;
        sscanf(str, "%d", &x);
        kill(x, SIGSTOP);
        return SUCCESS;
    }
    else if (strcmp(command->name, "mybg")==0)
    {
        //TODO: DONE
        //we send a continue signal to the given process
        //The command is running in the background
        const char *str = command->args[0];
        int x;
        sscanf(str, "%d", &x);
        kill(x, SIGCONT);
        command->background=true;
        return SUCCESS;
    }
    else if (strcmp(command->name, "myfg")==0)
    {
        //TODO: DONE
        //we send a continue signal to the given process
        //The command is running in the foreground, shellgibi will wait for the child to finish.
        const char *str = command->args[0];
        int x;
        sscanf(str, "%d", &x); // Using sscanf
        kill(x, SIGCONT);
        command->background=false;
        return SUCCESS;
    }
        ///Part III (b) END

        ///Part III (c) Psvis
    else if (strcmp(command->name, "psvis")==0)
    {
        //TODO: KINDA DONE!
        //We didn't use the kernel module in our implementation because there was no need
        //We create a process tree with pstree command in the system.
        char *p = command->args[0];
        char term[128];
        if(command->arg_count > 0){
            snprintf(term, sizeof(term), "pstree %s", p);
            system(term);
            return SUCCESS;
        }else{
            system("pstree");
        }
        return SUCCESS;
    }
        ///Part III (c) END

        ///Part III (d) alarm
    else if (strcmp(command->name, "alarm")==0)
    {
        //getting user input
        char cwd[1024];
        getcwd(cwd, sizeof(cwd));
        char *timeArgument = command->args[0];
        char *alarmFileName = command->args[1];
        if(command->arg_count <= 1){
            printf("alarm command arguments must match: time(e.g 7.14) file(e.g alarm.wav)\n");
            return 0;
        }
        char timeArray[2][5]; // cronjob requires 2 digit, 5 dates: minute hour day month weekday respectively
        char *p;
        int i = 0;
        p = strtok(timeArgument,"."); //Parse the given time into time and minute
        while (p != NULL)
        {
            //spliting the time (e.g 7.14 to hour 7 and minute 14)
            strcpy(timeArray[i],p);
            p = strtok(NULL, ".");
            i++;
        }
        //we put the alarm music into an executable file
        FILE *fpAlarm;
        fpAlarm = fopen("play.sh", "w");
        fprintf(fpAlarm, "play %s/%s trim 0.0 60", cwd, alarmFileName);
        fclose(fpAlarm);
        //we create a crontab file and create the cronjob here
        FILE *fpCrontab;
        fpCrontab = fopen("crontabFile", "w");
        fprintf(fpCrontab, "%s %s * * * %s /play.sh \n",timeArray[1],timeArray[0], cwd);
        fclose(fpCrontab);
        char* args[] = {"crontab","crontabFile",NULL};
        execv("/usr/bin/crontab", args);
        return SUCCESS;
    }
    ///Part III (d) END

    ///Part III (e) Custom command: Search
    // THIS COMMAND HELPS YOU FIND A PATH OF FILES IN YOUR USER,
    // REALLY GOOD FOR MESSY DEVELOPERS LIKE ME BECAUSE I LOSE STUFF
    // USUALLY SCREENSHOTS :) ~ MURAT
    if (strcmp(command->name, "search")==0)
    {
        char* buffer[10000];
        int i = 0;
        bool found = false;
        const char *homedir;

        void depthFirstSearch(const char *name, int depth, char *str)
        {
            DIR *dir;
            struct dirent *entry;
            if (!(dir = opendir(name)))
                return;
            while ((entry = readdir(dir)) != NULL) { /* In a depth-first manner */
                if (entry->d_type == DT_DIR) { /* if the file is a directory */
                    char path[1000];
                    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                        continue;
                    snprintf(path, sizeof(path), "%s/%s", name, entry->d_name);
                    if(strcmp(entry->d_name, str) == 0){
                        found = true;
                        for(i = 0; i < depth; i++){
                            if(found){
                                printf("%s/",buffer[i]);

                            }
                        }
                        break;
                    }
                    buffer[depth] = entry->d_name;
                    depthFirstSearch(path, depth + 1,str);
                } else { /* if not a directory */
                    if(strcmp(entry->d_name, str) == 0) {
                        found = true;
                        for (i = 0; i < depth; i++) {
                            if (found) {
                                printf("%s/", buffer[i]);

                            }
                        }
                        break;
                    }
                }
            }
            closedir(dir);
        }
        printf("/home/");
        homedir = getpwuid(getuid())->pw_dir;
        depthFirstSearch(homedir, 0, command->args[0]);
        if(!found) {
            printf(" is searched, but could not found.\n");
        } else {
            printf("%s\n",command->args[0]);
        }
        return 0;
    }
    ///Part III (e) Custom command: blank
    //Tic Tac Toe game for bored people like me
    if (strcmp(command->name, "blank")==0){

    }
    ///Part III (e) END
    ///---------------------------Part III END-------------------------------------
    int fd;
    pid_t pid=fork();
    if (pid==0) // child
    {

        /// This shows how to do exec with environ (but is not available on MacOs)
        // extern char** environ; // environment variables
        // execvpe(command->name, command->args, environ); // exec+args+path+environ

        /// This shows how to do exec with auto-path resolve
        // add a NULL argument to the end of args, and the name to the beginning
        // as required by exec

//I/O redirection

        if (command->redirects[1]!=NULL)
        {
            fd = open(command->redirects[1],  O_WRONLY | O_CREAT | O_TRUNC, "w");
            dup2(fd, 1);
            close(fd);
            command->args[1]=NULL;
        }
        else if (command->redirects[2]!=NULL)
        {
            fd = open(command->redirects[2], O_CREAT | O_WRONLY | O_APPEND, "a");
            dup2(fd, 1);
            close(fd);
            command->args[1]=NULL;
        }

        else if (command->redirects[0]!=NULL)
        {
            FILE *file;
            char input[100]={"input"};

            file=fopen(command->redirects[0],"r");
            fgets(input,100,file);
            input[strlen(input)-1]='\0';

            char *const *inputfile=(char *const *) input;

            execv(command->name, inputfile);
            fclose(file);

        }

        else if (fd < 0)
        {
            printf("Failed to create output file");
            exit(1);
        }

        // increase args size by 2
        command->args=(char **)realloc(
                command->args, sizeof(char *)*(command->arg_count+=2));

        // shift everything forward by 1
        for (int i=command->arg_count-2;i>0;--i)
            command->args[i]=command->args[i-1];

        // set args[0] as a copy of name
        command->args[0]=strdup(command->name);
        // set args[arg_count-1] (last) to NULL
        command->args[command->arg_count-1]=NULL;

//      execvp(command->name, command->args); // exec+args+path
//      exit(0);
        /// TODO: do your own exec with path resolving using execv()

        if(command->name[0]=='/' || command->name[0]=='.'){
            execv(command->name,command->args);
        }
        else if(strcmp(command->name,"gcc")==0){
            execv("/usr/bin/gcc", command->args);
        }
        else{
            char path[100]="/bin/";
            strcat(path,command->name);
            execv(path, command->args);
        }
    }

    else
    {
        if (!command->background)
            wait(0); // wait for child process to finish
        return SUCCESS;
    }

    // TODO: your implementation here
    //for to autocomplete

    struct dirent *de;
    DIR *dr=opendir(".");
    char directories[100][40];
    int i=0;

    if(dr==NULL){
        printf("could not open current directory \n");
        return 0;
    }

    while((de=readdir(dr))!=NULL){
        strcpy(directories[i],de->d_name);
        i++;
    }

    closedir(dr);

//for recursive pipng

    static int child = 0; // to see child or not

    void report_error_and_exit(const char* msg) {
        perror(msg);
        (child ? _exit : exit)(EXIT_FAILURE);
    }

// old to new
    void redirect(int oldfd, int newfd) {
        if (oldfd != newfd) {
            if (dup2(oldfd, newfd) != -1)
                close(oldfd);
            else
                report_error_and_exit("dup2");
        }
    }

    void exec_pipeline(char* const* cmds[], size_t pos, int in_fd) {
        if (cmds[pos + 1] == NULL) {
            redirect(in_fd, STDIN_FILENO);
            execvp(cmds[pos][0], cmds[pos]);
            report_error_and_exit("execvp last");
        }
        else {
            int fd[2];
            if (pipe(fd) == -1)
                report_error_and_exit("pipe");
            switch(fork()) {
                case -1:
                    report_error_and_exit("fork");
                case 0:
                    child = 1;
                    close(fd[0]);
                    redirect(in_fd, STDIN_FILENO);
                    redirect(fd[1], STDOUT_FILENO);
                    execvp(cmds[pos][0], cmds[pos]);
                    report_error_and_exit("execvp");
                default:
                    close(fd[1]);
                    close(in_fd);
                    exec_pipeline(cmds, pos + 1, fd[0]);
            }
        }
    }

    printf("-%s: %s: command not found\n", sysname, command->name);
    return UNKNOWN;
}
