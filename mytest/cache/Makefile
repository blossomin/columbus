OW_USER?=openwhisk
OW_VER?=v1.16:nightly
OW_RUNTIME?=$(OW_USER)/action-golang-$(OW_VER)
OW_COMPILER?=$(OW_USER)/action-golang-$(OW_VER)
WSK?=wsk
MAIN=main
PACKAGE=test
SRC=$(MAIN).go
NAME=single-main
ZIP=$(MAIN).zip

deploy: package.done $(ZIP)
	$(WSK) action update $(PACKAGE)/$(NAME) $(ZIP) --main $(MAIN) --docker $(OW_RUNTIME)

devel: package.done $(SRC)
	$(WSK) action update $(PACKAGE)/$(NAME) $(SRC) --main $(MAIN) --docker $(OW_COMPILER)

$(ZIP): $(SRC)
	docker run -i $(OW_COMPILER) -compile $(MAIN) <$(SRC) >$(ZIP)

clean:
	-$(WSK) action delete $(PACKAGE)/$(NAME)
	-rm $(ZIP) package.done test.json 2>/dev/null
	-rm test.out

test: test.json
	$(WSK) action invoke test/$(NAME) -r | tee -a test.out
	$(WSK) action invoke test/$(NAME) -P test.json -r | tee -a test.out

test.json:
	echo '{ "name": "Mike" }' >test.json

package.done:
	$(WSK) package update $(PACKAGE)
	touch package.done

.PHONY: deploy devel test clean