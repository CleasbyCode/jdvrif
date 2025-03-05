uint64_t getPin() {

    const std::string MAX_UINT64_STR = "18446744073709551615";

    std::string input;
    char ch; 
    bool sync_status = std::cout.sync_with_stdio(false);

#ifdef _WIN32
    while (input.length() < 20) { 
	 ch = _getch();
        if (ch >= '0' && ch <= '9') {
            input.push_back(ch);
            std::cout << '*' << std::flush;  
        } else if (ch == '\b' && !input.empty()) {  
            std::cout << "\b \b" << std::flush;  
            input.pop_back();
        } else if (ch == '\r') {
            break;
        }

    }
#else
   
    struct termios oldt, newt;

    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;

    newt.c_lflag &= ~(ICANON | ECHO);

    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

   while (input.length() < 20) {
        ssize_t bytes_read = read(STDIN_FILENO, &ch, 1); 
        if (bytes_read <= 0) continue; 
       
        if (ch >= '0' && ch <= '9') {
            input.push_back(ch);
            std::cout << '*' << std::flush; 
        } else if ((ch == '\b' || ch == 127) && !input.empty()) {  
            std::cout << "\b \b" << std::flush;
            input.pop_back();
        } else if (ch == '\n') {
            break;
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt); 

#endif

    std::cout << std::endl; 

    std::cout.sync_with_stdio(sync_status);
    uint64_t user_pin;
    if (input.empty() || (input.length() == 20 && input > MAX_UINT64_STR)) {
        user_pin = 0; 
    } else {
        user_pin = std::stoull(input); 
    }
    return user_pin;
}
