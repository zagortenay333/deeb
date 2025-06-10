--------------------------------------------------------------------------------
-- People
--------------------------------------------------------------------------------
create table People (
    id      int primary key,
    num     int not null,
    msg1    text,
    married bool,
    msg2    text
)

insert into People (0, 42, "No woman #1", true, "No cry #1")
insert into People (1, 6, "No woman #2\n\
                           and no woman...", true, "No cry #2\n\
                                                    and no cry...\n\
                                                    and still no cry...")
insert into People (2, 17, "No woman #2\n\
                            and no woman...\n\
                            and still no woman...", false, "No cry #2\n\
                                                            and still no cry...\n\n\
                                                            and evermore no cry...")
insert into People (3, 99, "", true, "No cry #3")
insert into People (4, 234, "No woman #4", false, "\n\n\n")

--------------------------------------------------------------------------------
-- Dudes
--------------------------------------------------------------------------------
create table Dudes (
    id   int primary key,
    num  int,
    msg1 text,
    msg2 text
)

insert into Dudes (0, 69, "The dudes are coming out.", "Duuuuuuuuuuuude.")
insert into Dudes (1, 9001, "I ain't scared.", "Ahhhhhhhhhhhhhhhhh.")
insert into Dudes (2, 420, "I'm the dude\n\
                            playing a dude\n\
                            disguised as another dude.", "You the dude\n\
                                                          who don't know what dude he is.")

--------------------------------------------------------------------------------
-- Queries
--------------------------------------------------------------------------------
update Dudes
set msg1 = "I am not father O'Malley"
where Dudes.id = 1

select People.id, People.msg1, Dudes.id, Dudes.msg1
from People join Dudes on (Dudes.id != 0) and (People.id > 1)

select * from Dudes

select 2 + 3 * 2, 9000 + 1, false != true, "I'm Batman!"

--------------------------------------------------------------------------------
-- Cleanup
--------------------------------------------------------------------------------
drop table Dudes
drop table People
